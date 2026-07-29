#include "gt06_parser.h"

#include <ctime>

namespace {

constexpr uint8_t kStartByte1 = 0x78;
constexpr uint8_t kStartByte2 = 0x78;
constexpr uint8_t kStopByte1 = 0x0D;
constexpr uint8_t kStopByte2 = 0x0A;

// GT06 packet type bytes this parser understands. Every other type byte
// (alarm variants, commands, extended-format LBS-only packets, ...) is
// still framed and checksum-validated correctly (so the connection doesn't
// desync), just returned as kUnsupported - out of scope for issue #123's
// "basic tracking" pass.
constexpr uint8_t kTypeLogin = 0x01;
constexpr uint8_t kTypeGpsLocation = 0x10;
constexpr uint8_t kTypeHeartbeat = 0x23;

// Minimum possible frame: start(2) + length(1) + type(1) + serial(2) +
// checksum(2) + stop(2), zero-length content.
constexpr size_t kMinFrameLength = 10;

// Standard (non-extended, swapFlags=false, longSpeed=false) GPS location
// content: date/time(6) + length-validity byte(1) + satellites(1) +
// latitude(4) + longitude(4) + speed(1) + course-and-flags(2) = 19 bytes.
constexpr size_t kLocationContentLength = 19;

// GT06 packs the IMEI as 8 bytes of BCD (two decimal digits per byte, 16
// digits of capacity for a 15-digit IMEI - the leading nibble is a padding
// zero). Decoded digit by digit rather than as a 64-bit integer so a
// leading zero nibble round-trips correctly instead of being silently
// dropped by numeric parsing.
std::string decode_imei(const std::span<const uint8_t> imei_bytes) {
    std::string imei;
    imei.reserve(imei_bytes.size() * 2);
    for (const uint8_t byte : imei_bytes) {
        const uint8_t high = (byte >> 4) & 0x0F;
        const uint8_t low = byte & 0x0F;
        imei.push_back(static_cast<char>('0' + high));
        imei.push_back(static_cast<char>('0' + low));
    }
    // Traccar strips a single leading '0' padding digit down to the real
    // 15-digit IMEI; every real-world GT06 IMEI is 15 digits, so a 16-digit
    // decode with a leading zero is the padding case, not a real 16-digit
    // IMEI in its own right.
    if (imei.size() == 16 && imei.front() == '0') {
        imei.erase(imei.begin());
    }
    return imei;
}

// Builds a unix epoch millisecond timestamp from GT06's YY MM DD HH MM SS
// fields (UTC - GT06 devices report time in UTC, no timezone field in the
// standard login/location packets this parser handles). year_since_2000 is
// the raw byte GT06 sends (e.g. 24 for 2024); every fielded GT06 device is
// well within the 2000-2099 range this covers.
uint64_t build_timestamp_ms(const uint8_t year_since_2000, const uint8_t month, const uint8_t day,
                             const uint8_t hour, const uint8_t minute, const uint8_t second) {
    std::tm tm{};
    tm.tm_year = year_since_2000 + 100;  // tm_year is years since 1900
    tm.tm_mon = month - 1;               // tm_mon is 0-11
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
#if defined(_WIN32)
    const std::time_t epoch_seconds = _mkgmtime(&tm);
#else
    const std::time_t epoch_seconds = timegm(&tm);
#endif
    return static_cast<uint64_t>(epoch_seconds) * 1000;
}

}  // namespace

uint16_t Gt06Parser::crc16_x25(const std::span<const uint8_t> data) {
    // Reflected CRC-16/X-25 (poly 0x1021 non-reflected == 0x8408 reflected,
    // init 0xFFFF, refin/refout true, xorout 0xFFFF) - ported from
    // Traccar's Checksum.java CRC16_X25 algorithm definition. The same
    // well-known variant used by PPP/HDLC framing; this is the canonical
    // bit-shift form, not something invented here.
    uint16_t crc = 0xFFFF;
    for (const uint8_t byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 1) {
                crc = static_cast<uint16_t>((crc >> 1) ^ 0x8408);
            } else {
                crc = static_cast<uint16_t>(crc >> 1);
            }
        }
    }
    return static_cast<uint16_t>(crc ^ 0xFFFF);
}

Gt06Parser::ParseResult Gt06Parser::parse_frame(const std::span<const uint8_t> data) {
    if (data.size() < kMinFrameLength) {
        return {FrameResult::kIncomplete};
    }
    if (data[0] != kStartByte1 || data[1] != kStartByte2) {
        return {FrameResult::kInvalid};
    }

    const uint8_t length_byte = data[2];
    // length_byte covers type(1) + content(N) + serial(2) + checksum(2), so
    // it can never be below 5 (N == 0) in a real frame.
    if (length_byte < 5) {
        return {FrameResult::kInvalid};
    }
    const size_t content_length = static_cast<size_t>(length_byte) - 5;
    const size_t total_frame_length = static_cast<size_t>(length_byte) + 5;
    if (data.size() < total_frame_length) {
        return {FrameResult::kIncomplete};
    }
    if (data[total_frame_length - 2] != kStopByte1 || data[total_frame_length - 1] != kStopByte2) {
        return {FrameResult::kInvalid};
    }

    const uint8_t type_byte = data[3];
    const std::span<const uint8_t> content = data.subspan(4, content_length);
    const size_t serial_offset = 4 + content_length;
    const auto serial_number = static_cast<uint16_t>((data[serial_offset] << 8) | data[serial_offset + 1]);
    const size_t checksum_offset = serial_offset + 2;
    const auto received_checksum =
        static_cast<uint16_t>((data[checksum_offset] << 8) | data[checksum_offset + 1]);

    // CRC covers the length byte through the end of the serial number -
    // everything written so far except the two start bytes and the
    // checksum/stop bytes that follow it (matches Traccar's own
    // nioBuffer(2, writerIndex - 2) at the point the checksum is computed,
    // before checksum/stop are appended).
    const uint16_t computed_checksum = crc16_x25(data.subspan(2, checksum_offset - 2));
    if (computed_checksum != received_checksum) {
        return {FrameResult::kInvalid};
    }

    auto packet = parse_content(type_byte, content, serial_number);
    if (!packet.has_value()) {
        return {FrameResult::kInvalid};
    }
    return {FrameResult::kOk, total_frame_length, *packet};
}

std::optional<Gt06Parser::ParsedPacket> Gt06Parser::parse_content(const uint8_t type_byte,
                                                                    const std::span<const uint8_t> content,
                                                                    const uint16_t serial_number) {
    switch (type_byte) {
        case kTypeLogin: {
            // 8 bytes IMEI, optionally followed by a 2-byte device type and
            // (rarer) a 2-byte timezone extension - only the IMEI matters
            // here, so anything shorter than 8 bytes is malformed rather
            // than just "missing the optional fields".
            if (content.size() < 8) return std::nullopt;
            ParsedPacket packet;
            packet.type = PacketType::kLogin;
            packet.serial_number = serial_number;
            packet.imei = decode_imei(content.subspan(0, 8));
            return packet;
        }
        case kTypeGpsLocation: {
            if (content.size() < kLocationContentLength) return std::nullopt;
            const uint8_t length_validity = content[6];
            if (length_validity == 0) return std::nullopt;
            const uint8_t satellites = content[7] & 0x0F;
            auto read_u32 = [&](const size_t offset) {
                return (static_cast<uint32_t>(content[offset]) << 24) |
                       (static_cast<uint32_t>(content[offset + 1]) << 16) |
                       (static_cast<uint32_t>(content[offset + 2]) << 8) |
                       static_cast<uint32_t>(content[offset + 3]);
            };
            double latitude_deg = static_cast<double>(read_u32(8)) / 60.0 / 30000.0;
            double longitude_deg = static_cast<double>(read_u32(12)) / 60.0 / 30000.0;
            // Standard (non-extended) location packet: 1-byte speed at
            // offset 16, then the 2-byte course-and-flags field at 17-18
            // (swapFlags=false: flags come after speed, not before).
            const auto flags = static_cast<uint16_t>((content[17] << 8) | content[18]);
            const bool gps_valid = (flags & (1u << 12)) != 0;
            const bool latitude_is_north = (flags & (1u << 10)) != 0;
            const bool longitude_is_west = (flags & (1u << 11)) != 0;
            if (!latitude_is_north) latitude_deg = -latitude_deg;
            if (longitude_is_west) longitude_deg = -longitude_deg;

            ParsedPacket packet;
            packet.type = PacketType::kLocation;
            packet.serial_number = serial_number;
            packet.location = Location{
                .timestamp_ms = build_timestamp_ms(content[0], content[1], content[2], content[3],
                                                     content[4], content[5]),
                .latitude_deg = latitude_deg,
                .longitude_deg = longitude_deg,
                .gps_valid = gps_valid,
                .satellites = satellites,
            };
            return packet;
        }
        case kTypeHeartbeat: {
            ParsedPacket packet;
            packet.type = PacketType::kHeartbeat;
            packet.serial_number = serial_number;
            return packet;
        }
        default: {
            ParsedPacket packet;
            packet.type = PacketType::kUnsupported;
            packet.serial_number = serial_number;
            return packet;
        }
    }
}

std::vector<uint8_t> Gt06Parser::build_ack(const PacketType type, const uint16_t serial_number) {
    if (type == PacketType::kUnsupported) return {};

    uint8_t type_byte = 0;
    switch (type) {
        case PacketType::kLogin:
            type_byte = kTypeLogin;
            break;
        case PacketType::kLocation:
            // GT06 devices do not expect (and most firmwares do not wait
            // for) an ack per location packet the way they do for login/
            // heartbeat; returning empty here means Gt06TcpServer sends
            // nothing back for a location packet, which matches Traccar's
            // own behavior for the base GPS message type.
            return {};
        case PacketType::kHeartbeat:
            type_byte = kTypeHeartbeat;
            break;
        case PacketType::kUnsupported:
            return {};
    }

    // Empty-content ack: start(2) + length(1) + type(1) + serial(2) +
    // checksum(2) + stop(2). length_byte = type(1)+serial(2)+checksum(2) = 5.
    std::vector<uint8_t> frame = {
        kStartByte1, kStartByte2, 5, type_byte,
        static_cast<uint8_t>(serial_number >> 8), static_cast<uint8_t>(serial_number & 0xFF),
    };
    const uint16_t checksum = Gt06Parser::crc16_x25(std::span(frame).subspan(2));
    frame.push_back(static_cast<uint8_t>(checksum >> 8));
    frame.push_back(static_cast<uint8_t>(checksum & 0xFF));
    frame.push_back(kStopByte1);
    frame.push_back(kStopByte2);
    return frame;
}
