#include "gt06_parser.h"

#include <ctime>

namespace {

constexpr uint8_t kStartByte1 = 0x78;
constexpr uint8_t kStartByte2 = 0x78;
// Extended frame: identical framing otherwise, just a 2-byte length field
// instead of 1-byte (Traccar's own decode(): "extended = buf.readShort() !=
// 0x7878", "length = extended ? readUnsignedShort() : readUnsignedByte()")
// - used for payloads too large for a standard frame's 255-byte length
// field (WiFi/BLE positioning, ICCID, OBD, photo/file metadata). github#283
// found this is common in real traffic, not an edge case: 44 of 185 real
// device captures used this header.
constexpr uint8_t kExtendedStartByte1 = 0x79;
constexpr uint8_t kExtendedStartByte2 = 0x79;
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
    // Need both start bytes before extended-ness (and therefore the real
    // minimum frame length) can even be determined.
    if (data.size() < 2) {
        return {FrameResult::kIncomplete};
    }
    bool extended;
    if (data[0] == kStartByte1 && data[1] == kStartByte2) {
        extended = false;
    } else if (data[0] == kExtendedStartByte1 && data[1] == kExtendedStartByte2) {
        extended = true;
    } else {
        return {FrameResult::kInvalid};
    }

    // Standard: 1-byte length field. Extended: 2-byte length field (same
    // value range it describes - type(1)+content(N)+serial(2)+checksum(2) -
    // just wide enough for payloads a standard frame's 255-byte field
    // can't address). Only need enough bytes to *read* the length field
    // here - not a "normal" full-size frame's worth. An independent review
    // of this fix (github#283) caught a real bug in an earlier version of
    // this check: gating on a fixed 10/11-byte minimum rejected a genuine,
    // complete, correctly-terminated 6-byte real-device capture
    // (0x7878 0x01 0x08 0x0D 0x0A - length_value 1, degenerate but real) as
    // permanently kIncomplete, since it never reached the degenerate-length
    // handling below at all. Silently losing a real frame if the
    // connection ends before more bytes ever arrive violates this repo's
    // own "every failure is observable" rule.
    const size_t length_field_width = extended ? 2 : 1;
    if (data.size() < 2 + length_field_width) {
        return {FrameResult::kIncomplete};
    }
    const size_t length_value =
        extended ? (static_cast<size_t>(data[2]) << 8) | data[3] : data[2];

    // Find the frame's real end before trusting length_value for anything
    // else: github#283 found real device captures where the declared
    // length is simply wrong - one off by a single byte short, one three
    // bytes short of a real 15-byte frame (so "not enough bytes yet for the
    // declared length" does NOT mean the frame is actually incomplete; it
    // can already be a complete, shorter real frame), one a flat-out
    // nonsensical 0. Traccar's own Gt06FrameDecoder.java doesn't special-
    // case any of these differently - whenever the fast-path position
    // doesn't land exactly on a 0x0D 0x0A stop marker, for *any* reason, it
    // unconditionally falls back to scanning forward for the next literal
    // one. Matched exactly here, including paying the same O(buffer size)
    // scan cost Traccar's own reference decoder pays in this situation -
    // an earlier version of this fix tried to skip the scan specifically
    // when the buffer was merely shorter than the declared length, as a
    // performance optimization, but that silently broke the three-bytes-
    // short real capture above (it already contains a complete frame
    // shorter than its own declared length, findable only by scanning).
    // Buffer growth itself is bounded by the caller (Gt06TcpServer), the
    // same layer Traccar's own BaseFrameDecoder enforces its 32KB cap at,
    // not here - this stays pure parsing logic over whatever it's given.
    size_t total_frame_length = 2 + length_field_width + length_value + 2;
    const bool declared_length_checks_out = length_value >= 5 && data.size() >= total_frame_length &&
                                             data[total_frame_length - 2] == kStopByte1 &&
                                             data[total_frame_length - 1] == kStopByte2;
    if (!declared_length_checks_out) {
        std::optional<size_t> stop_marker_end;
        for (size_t i = 0; i + 1 < data.size(); ++i) {
            if (data[i] == kStopByte1 && data[i + 1] == kStopByte2) {
                stop_marker_end = i + 2;
                break;
            }
        }
        if (!stop_marker_end.has_value()) {
            // Could still be a genuine prefix (more bytes on the way) or
            // could be truly malformed - same ambiguity Traccar's own frame
            // decoder has here (it returns null either way, waiting for
            // more). Keep waiting rather than guess.
            return {FrameResult::kIncomplete};
        }
        total_frame_length = *stop_marker_end;
    }

    // The frame's real boundary is solid either way at this point. What
    // follows - type/content/serial/checksum extraction, and parse_content's
    // own per-type layout checks - all still depends on length_value, which
    // github#283 found real devices get wrong in more than one way (a flat
    // 0, three bytes short of what a real 15-byte frame needed, a location
    // packet declared shorter than this parser's fixed 19-byte layout).
    // Traccar's own decode() hits the same wall in each case - its
    // dataLength-based reads either go negative (a ByteBuf exception its
    // pipeline catches higher up) or under-read a fixed-layout field
    // (decodeGps's own length-validity-byte check) - and its test suite
    // still expects these captures to decode without throwing, just not
    // necessarily into a meaningful Position. One consistent rule here
    // mirrors that: anything past this point that can't be made sense of
    // falls back to kUnsupported, never kInvalid - the stream is already
    // known to be in sync (a real stop marker was just found), so there is
    // nothing left to protect by dropping the connection over it.
    const size_t content_length = length_value < 5 ? 0 : length_value - 5;
    const size_t type_offset = 2 + length_field_width;
    const size_t content_offset = type_offset + 1;
    const size_t serial_offset = content_offset + content_length;
    const size_t checksum_offset = serial_offset + 2;
    const bool offsets_fit_in_frame = length_value >= 5 && checksum_offset + 2 <= data.size();

    if (!offsets_fit_in_frame) {
        ParsedPacket packet;
        packet.type = PacketType::kUnsupported;
        // Can't reliably read a serial number either - the bytes it would
        // come from may not even exist within this frame when the declared
        // length is this degenerate. 0 is a plain, honest "unknown", not a
        // claim this is really serial 0.
        packet.serial_number = 0;
        return {FrameResult::kOk, total_frame_length, packet};
    }

    const uint8_t type_byte = data[type_offset];
    const std::span<const uint8_t> content = data.subspan(content_offset, content_length);
    const auto serial_number = static_cast<uint16_t>((data[serial_offset] << 8) | data[serial_offset + 1]);
    const auto received_checksum =
        static_cast<uint16_t>((data[checksum_offset] << 8) | data[checksum_offset + 1]);

    // CRC covers the length field through the end of the serial number -
    // everything written so far except the two start bytes and the
    // checksum/stop bytes that follow it (matches Traccar's own
    // nioBuffer(2, writerIndex - 2) at the point the checksum is computed,
    // before checksum/stop are appended) - same range rule for either
    // framing, it just starts one byte later when extended.
    const uint16_t computed_checksum = crc16_x25(data.subspan(2, checksum_offset - 2));
    // A mismatch here is deliberately not kInvalid - see FrameResult's own
    // doc comment (github#283): real devices do this, and Traccar's
    // reference decoder never checks it either. Still decoded as normal,
    // just flagged for the caller to log if it wants to.
    const bool checksum_mismatch = computed_checksum != received_checksum;

    auto packet = parse_content(type_byte, content, serial_number);
    if (!packet.has_value()) {
        // parse_content only returns nullopt for a type it does recognize
        // but whose declared content is too short for that type's fixed
        // layout (or, for location, an explicit length_validity == 0) -
        // "recognized type, content this parser can't fully use," the same
        // category as everything else handled above. kUnsupported, not
        // kInvalid, for the same reason.
        ParsedPacket unsupported;
        unsupported.type = PacketType::kUnsupported;
        unsupported.serial_number = serial_number;
        return {FrameResult::kOk, total_frame_length, unsupported};
    }
    ParseResult result{FrameResult::kOk, total_frame_length, *packet};
    result.checksum_mismatch = checksum_mismatch;
    return result;
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
