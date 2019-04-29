#include <DataStreams/copyData.h>
#include <IO/ReadBufferFromPocoSocket.h>
#include <IO/WriteBufferFromPocoSocket.h>
#include <Interpreters/executeQuery.h>
#include <Storages/IStorage.h>
#include <Core/MySQLProtocol.h>
#include <Core/NamesAndTypes.h>
#include <Columns/ColumnVector.h>
#include <Common/config_version.h>
#include <Common/NetException.h>
#include <Poco/Crypto/RSAKey.h>
#include <Poco/Crypto/CipherFactory.h>
#include <Poco/Net/SecureStreamSocket.h>
#include <Poco/Net/SSLManager.h>
#include "MySQLHandler.h"
#include <limits>


namespace DB
{
using namespace MySQLProtocol;
using Poco::Net::SecureStreamSocket;
using Poco::Net::SSLManager;

namespace ErrorCodes
{
    extern const int MYSQL_CLIENT_INSUFFICIENT_CAPABILITIES;
    extern const int UNKNOWN_EXCEPTION;
}

uint32_t MySQLHandler::last_connection_id = 0;


void MySQLHandler::run()
{
    connection_context = server.context();

    packet_sender = PacketSender(socket());

    try
    {
        String scramble = generateScramble();

        /** Native authentication sent 20 bytes + '\0' character = 21 bytes.
         *  This plugin must do the same to stay consistent with historical behavior if it is set to operate as a default plugin.
         *  https://github.com/mysql/mysql-server/blob/8.0/sql/auth/sql_authentication.cc#L3994
         */
        Handshake handshake(connection_id, VERSION_STRING, scramble + '\0');

        packet_sender.sendPacket<Handshake>(handshake, true);

        LOG_TRACE(log, "Sent handshake");

        HandshakeResponse handshake_response = finishHandshake();

        LOG_DEBUG(log, "Capabilities: " << handshake_response.capability_flags
                                        << "\nmax_packet_size: "
                                        << handshake_response.max_packet_size
                                        << "\ncharacter_set: "
                                        << handshake_response.character_set
                                        << "\nuser: "
                                        << handshake_response.username
                                        << "\nauth_response length: "
                                        << handshake_response.auth_response.length()
                                        << "\nauth_response: "
                                        << handshake_response.auth_response
                                        << "\ndatabase: "
                                        << handshake_response.database
                                        << "\nauth_plugin_name: "
                                        << handshake_response.auth_plugin_name);

        capabilities = handshake_response.capability_flags;
        if (!(capabilities & CLIENT_PROTOCOL_41))
        {
            throw Exception("Required capability: CLIENT_PROTOCOL_41.", ErrorCodes::MYSQL_CLIENT_INSUFFICIENT_CAPABILITIES);
        }
        if (!(capabilities & CLIENT_PLUGIN_AUTH))
        {
            throw Exception("Required capability: CLIENT_PLUGIN_AUTH.", ErrorCodes::MYSQL_CLIENT_INSUFFICIENT_CAPABILITIES);
        }

        authenticate(handshake_response, scramble);
        OK_Packet ok_packet(0, handshake_response.capability_flags, 0, 0, 0, 0, "");
        packet_sender.sendPacket(ok_packet, true);

        while (true)
        {
            packet_sender.resetSequenceId();
            String payload = packet_sender.receivePacketPayload();
            int command = payload[0];
            LOG_DEBUG(log, "Received command: " << std::to_string(command) << ". Connection id: " << connection_id << ".");
            try
            {
                switch (command)
                {
                    case COM_QUIT:
                        return;
                    case COM_INIT_DB:
                        comInitDB(payload);
                        break;
                    case COM_QUERY:
                        comQuery(payload);
                        break;
                    case COM_FIELD_LIST:
                        comFieldList(payload);
                        break;
                    case COM_PING:
                        comPing();
                        break;
                    default:
                        throw Exception(Poco::format("Command %d is not implemented.", command), ErrorCodes::NOT_IMPLEMENTED);
                }
            }
            catch (const NetException & exc)
            {
                log->log(exc);
                throw;
            }
            catch (const Exception & exc)
            {
                log->log(exc);
                packet_sender.sendPacket(ERR_Packet(exc.code(), "00000", exc.message()), true);
            }
        }
    }
    catch (Poco::Exception & exc)
    {
        log->log(exc);
    }
}

MySQLProtocol::HandshakeResponse MySQLHandler::finishHandshake()
{
    /** Size of SSLRequest packet is 32 bytes.
     *  If we read more, then we will read part of SSL handshake, and it will be impossible to start SSL connection using Poco.
     */
    HandshakeResponse packet;
    char b[100]; /// Client can send either SSLRequest or HandshakeResponse.
    size_t pos = 0;
    while (pos < 3) {
        int ret = socket().receiveBytes(b + pos, 36 - pos);
        if (ret == 0) {
            throw Exception("Cannot read all data. Bytes read: " + std::to_string(pos) + ". Bytes expected: 36.", ErrorCodes::CANNOT_READ_ALL_DATA);
        }
        pos += ret;
    }

    size_t packet_size = *reinterpret_cast<uint32_t *>(b) & 0xFFFFFF;
    LOG_TRACE(log, "packet size: " << packet_size);
    if (packet_size == 32) {
        ss = std::make_shared<SecureStreamSocket>(SecureStreamSocket::attach(socket(), SSLManager::instance().defaultServerContext()));
        packet_sender = PacketSender(*ss, 2);
        secure_connection = true;
        packet_sender.receivePacket(packet);
    } else {
        while (pos < 4 + packet_size) {
            int ret = socket().receiveBytes(b + pos, 4 + packet_size - pos);
            if (ret == 0) {
                throw Exception("Cannot read all data. Bytes read: " + std::to_string(pos) + ". Bytes expected: " + std::to_string(4 + packet_size) + ".", ErrorCodes::CANNOT_READ_ALL_DATA);
            }
            pos += ret;
        }
        packet.readPayload(std::string(b + 4, packet_size));
        packet_sender.sequence_id++;
    }
    return packet;
}

String MySQLHandler::generateScramble()
{
    String scramble(MySQLProtocol::SCRAMBLE_LENGTH, 0);
    Poco::RandomInputStream generator;
    for (size_t i = 0; i < scramble.size(); i++) {
        generator >> scramble[i];
    }
    return scramble;
}

void MySQLHandler::authenticate(const HandshakeResponse & handshake_response, const String & scramble) {

    String auth_response;
    AuthSwitchResponse response;
    if (handshake_response.auth_plugin_name != Authentication::CachingSHA2) {
        packet_sender.sendPacket(AuthSwitchRequest(Authentication::CachingSHA2, scramble + '\0'), true);
        packet_sender.receivePacket(response);
        auth_response = response.value;
        LOG_TRACE(log, "Authentication method mismatch.");
    }
    else
    {
        auth_response = handshake_response.auth_response;
        LOG_TRACE(log, "Authentication method match.");
    }

    /// Caching SHA2 plugin is used instead of SHA256 only because it can work without OpenSLL.
    /// Fast auth path is not used, because otherwise it would be possible to authenticate using data from users.xml.
    packet_sender.sendPacket(AuthMoreData("\4"), true);

    packet_sender.receivePacket(response);
    auth_response = response.value;

    auto getOpenSSLError = []() -> String {
        BIO * mem = BIO_new(BIO_s_mem());
        ERR_print_errors(mem);
        char * buf = nullptr;
        long size = BIO_get_mem_data(mem, &buf);
        String errors_str(buf, size);
        BIO_free(mem);
        return errors_str;
    };

    if (auth_response == "\2")
    {
        LOG_TRACE(log, "Client requests public key.");
        /// Client requests public key.
        BIO * mem = BIO_new(BIO_s_mem());
        if (PEM_write_bio_RSA_PUBKEY(mem, public_key) != 1)
        {
            LOG_TRACE(log, "OpenSSL error:\n" << getOpenSSLError());
            throw Exception("Failed to write public key to memory.", ErrorCodes::UNKNOWN_EXCEPTION);
        }
        char * pem_buf = nullptr;
        long pem_size = BIO_get_mem_data(mem, &pem_buf);
        String pem(pem_buf, pem_size);
        BIO_free(mem);

        LOG_TRACE(log, "Key: " << pem);

        AuthMoreData data(pem);
        packet_sender.sendPacket(data, true);
        packet_sender.receivePacket(response);
        auth_response = response.value;
    }
    else
    {
        LOG_TRACE(log, "Client didn't request public key.");
    }

    String password;

    LOG_TRACE(log, "auth response: " << auth_response);

    /** Decrypt password, if it's not empty.
     *  The original intention was that the password is a string[NUL] but this never got enforced properly so now we have to accept that
     *  an empty packet is a blank password, thus the check for auth_response.empty() has to be made too.
     *  https://github.com/mysql/mysql-server/blob/8.0/sql/auth/sql_authentication.cc#L4017
     */

    if (!secure_connection && !(auth_response.empty() || (auth_response.size() == 1 && auth_response[0] == '\0')))
    {
        LOG_TRACE(log, "Received nonempty password");
        auto ciphertext = reinterpret_cast<unsigned char *>(auth_response.data());

        unsigned char plaintext[RSA_size(private_key)];
        int plaintext_size = RSA_private_decrypt(auth_response.size(), ciphertext, plaintext, private_key, RSA_PKCS1_OAEP_PADDING);
        if (plaintext_size == -1)
        {
            LOG_TRACE(log, "OpenSSL error:\n" << getOpenSSLError());
            throw Exception("Failed to decrypt.", ErrorCodes::UNKNOWN_EXCEPTION);
        }

        password.resize(plaintext_size);
        for (int i = 0; i < plaintext_size; i++) {
            password[i] = plaintext[i] ^ static_cast<unsigned char>(scramble[i % scramble.size()]);
        }
    }
    else if (secure_connection)
    {
        password = auth_response;
    }
    else
    {
        LOG_TRACE(log, "Received empty password");
    }

    if (!password.empty()) {
        /// remove terminating null byte
        password.pop_back();
    }

    try
    {
        connection_context.setUser(handshake_response.username, password, socket().address(), "");
        connection_context.setCurrentDatabase(handshake_response.database);
        connection_context.setCurrentQueryId("");
        LOG_ERROR(log, "Authentication for user " << handshake_response.username << " succeeded.");
    }
    catch (const Exception & exc)
    {
        LOG_ERROR(log, "Authentication for user " << handshake_response.username << " failed.");
        packet_sender.sendPacket(ERR_Packet(exc.code(), "00000", exc.message()), true);
        throw;
    }
}

void MySQLHandler::comInitDB(String payload)
{
    String database = payload.substr(1);
    LOG_DEBUG(log, "Setting current database to " << database);
    connection_context.setCurrentDatabase(database);
    packet_sender.sendPacket(OK_Packet(0, capabilities, 0, 0, 0, 1, ""), true);
}

void MySQLHandler::comFieldList(String payload)
{
    ComFieldList packet;
    packet.readPayload(payload);
    String database = connection_context.getCurrentDatabase();
    StoragePtr tablePtr = connection_context.getTable(database, packet.table);
    for (const NameAndTypePair & column: tablePtr->getColumns().getAll())
    {
        ColumnDefinition column_definition(
            database, packet.table, packet.table, column.name, column.name, CharacterSet::binary, 100, ColumnType::MYSQL_TYPE_STRING, 0, 0
        );
        packet_sender.sendPacket(column_definition);
    }
    packet_sender.sendPacket(OK_Packet(0xfe, capabilities, 0, 0, 0, 0, ""), true);
}

void MySQLHandler::comPing()
{
    packet_sender.sendPacket(OK_Packet(0x0, capabilities, 0, 0, 0, 0, ""), true);
}

void MySQLHandler::comQuery(String payload)
{
    BlockIO res = executeQuery(payload.substr(1), connection_context);
    FormatSettings format_settings;
    if (res.in)
    {
        LOG_TRACE(log, "Executing query with output.");

        Block header = res.in->getHeader();
        packet_sender.sendPacket(LengthEncodedNumber(header.columns()));

        for (const ColumnWithTypeAndName & column : header.getColumnsWithTypeAndName())
        {
            ColumnDefinition column_definition(column.name, CharacterSet::binary, std::numeric_limits<uint32_t>::max(),
                                               ColumnType::MYSQL_TYPE_STRING, 0, 0);
            packet_sender.sendPacket(column_definition);

            LOG_TRACE(log, "Sent " << column.name << " column definition");
        }

        LOG_TRACE(log, "Sent columns definitions.");

        if (!(capabilities & Capability::CLIENT_DEPRECATE_EOF))
        {
            packet_sender.sendPacket(EOF_Packet(0, 0));
        }

        while (Block block = res.in->read())
        {
            size_t rows = block.rows();

            for (size_t i = 0; i < rows; i++)
            {
                ResultsetRow row_packet;
                for (ColumnWithTypeAndName & column : block)
                {
                    column.column = column.column->convertToFullColumnIfConst();

                    String column_value;
                    WriteBufferFromString ostr(column_value);

                    LOG_TRACE(log, "Sending value of type " << column.type->getName() << " of column " << column.column->getName());

                    column.type->serializeAsText(*column.column.get(), i, ostr, format_settings);
                    ostr.finish();

                    row_packet.appendColumn(std::move(column_value));
                }
                packet_sender.sendPacket(row_packet);
            }
        }

        LOG_TRACE(log, "Sent rows.");
    }

    if (capabilities & CLIENT_DEPRECATE_EOF)
    {
        packet_sender.sendPacket(OK_Packet(0xfe, capabilities, 0, 0, 0, 0, ""), true);
    }
    else
    {
        packet_sender.sendPacket(EOF_Packet(0, 0), true);
    }
}

}
