#ifndef KARSHIPTA_GATEWAY_GT06_PARSER_H
#define KARSHIPTA_GATEWAY_GT06_PARSER_H

#include <cstdint>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Pure parsing logic for the GT06 tracker protocol: a raw binary TCP
// protocol (0x78 0x78 frame header, conventionally port 5023), not HTTP -
// the wire format hundreds of low-cost GPS tracker models speak natively
// (Concox/Jimi/iTrack family and countless OEM rebrands). Kept separate
// from Gt06TcpServer (socket/threading, IMEI-per-connection state) so
// framing/checksum/packet decoding is unit-testable with plain byte
// buffers and no real socket - the same split HeraldHttpServer/
// HeraldWardManager already have from each other.
//
// Reference: Traccar's Gt06ProtocolDecoder.java (github.com/traccar/traccar)
// and its Checksum.java (CRC16_X25 parameters) - ported, not
// reverse-engineered from nothing. This class only implements the standard
// (non-extended, 0x7878-framed) subset needed for login, basic GPS
// location, and heartbeat - the "basic tracking" scope in issue #123, not
// GT06's full command/alarm surface.
class Gt06Parser {
   public:
    enum class PacketType { kLogin, kLocation, kHeartbeat, kUnsupported };

    // A decoded GPS fix, before entity_id is known - GT06 location packets
    // never carry the IMEI themselves (it is sent once, in the login packet,
    // then implied for the rest of that TCP connection); Gt06TcpServer is
    // the one that remembers a connection's IMEI and combines it with this
    // to build a herald::v0::Herald message.
    struct Location {
        uint64_t timestamp_ms;
        double latitude_deg;
        double longitude_deg;
        bool gps_valid;
        uint32_t satellites;
    };

    struct ParsedPacket {
        PacketType type;
        uint16_t serial_number;
        // Only set when type == kLogin: the device's IMEI, decoded from
        // 8 BCD-packed bytes (each byte holds two decimal digits) into its
        // plain decimal string form, e.g. "861845041234567".
        std::optional<std::string> imei;
        // Only set when type == kLocation.
        std::optional<Location> location;
    };

    // kIncomplete and kInvalid need different caller behavior, not just a
    // single "didn't parse": kIncomplete means data is a valid-so-far
    // prefix that just needs more bytes from the next recv() (keep
    // buffering, do nothing else); kInvalid means the buffer does not start
    // with a real GT06 frame at all (bad start marker) or a complete
    // frame's checksum failed (real corruption) - the caller should log and
    // drop the connection rather than wait forever for bytes that will
    // never produce a valid frame.
    enum class FrameResult { kOk, kIncomplete, kInvalid };

    struct ParseResult {
        FrameResult result;
        // Only meaningful when result == kOk: the frame's total length
        // (start bytes through the trailing 0x0D 0x0A) - the caller must
        // remove exactly this many bytes from the front of its read buffer
        // before parsing again, since one recv() can deliver more than one
        // frame, or a partial one.
        size_t consumed_bytes = 0;
        // Only meaningful when result == kOk.
        ParsedPacket packet{};
    };

    // Parses exactly one frame starting at the beginning of `data`. Never
    // throws (gateway/CLAUDE.md rule 5: every failure observable via
    // FrameResult, not a crash or a silent drop).
    static ParseResult parse_frame(std::span<const uint8_t> data);

    // Builds the exact ack frame bytes GT06 expects back for a given packet
    // type/serial number, or empty for a type this parser does not
    // acknowledge (kUnsupported). Devices that get no ack for login or
    // heartbeat will retry and eventually drop the connection, so this is
    // required, not cosmetic.
    static std::vector<uint8_t> build_ack(PacketType type, uint16_t serial_number);

    // Exposed for testing against known-good CRC values from real device
    // captures without needing a full frame. CRC16/X-25: polynomial
    // 0x1021, init 0xFFFF, input and output reflected, final XOR 0xFFFF -
    // ported from Traccar's Checksum.java (CRC16_X25), the same well-known
    // variant used by PPP/HDLC framing.
    static uint16_t crc16_x25(std::span<const uint8_t> data);

   private:
    static std::optional<ParsedPacket> parse_content(uint8_t type_byte,
                                                       std::span<const uint8_t> content,
                                                       uint16_t serial_number);
};

#endif  // KARSHIPTA_GATEWAY_GT06_PARSER_H
