//! RSP packet parsing and formatting helpers.
//!
//! Covered subset (PR-2):
//!   g, G          — read/write all core registers
//!   m addr,len    — read memory
//!   M addr,len:xx — write memory
//!   s             — single step
//!   c             — continue
//!   Z1/z1         — hardware breakpoint set/clear
//!   Z2-Z4/z2-z4   — unsupported until DWT comparators are implemented
//!
//! All other commands get an empty reply (""), which is the RSP "not
//! supported" response per the GDB remote serial protocol spec.

/// Parse the first complete RSP packet from `buf`.
///
/// ACK and raw Ctrl-C events are returned in stream order with packets.
#[derive(Debug, PartialEq, Eq)]
pub enum PacketParse {
    Incomplete,
    Ack { consumed: usize },
    Interrupt { consumed: usize },
    Invalid { consumed: usize },
    Complete { data: String, consumed: usize },
}

pub fn parse_rsp_packet(buf: &[u8]) -> PacketParse {
    let start = match buf
        .iter()
        .position(|&b| b == b'$' || b == 0x03 || b == b'+')
    {
        Some(start) if buf[start] == b'+' => {
            return PacketParse::Ack {
                consumed: start + 1,
            };
        }
        Some(start) if buf[start] == 0x03 => {
            return PacketParse::Interrupt {
                consumed: start + 1,
            };
        }
        Some(start) => start,
        None => return PacketParse::Incomplete,
    };
    let hash = match buf[start + 1..].iter().position(|&b| b == b'#') {
        Some(offset) => start + 1 + offset,
        None => return PacketParse::Incomplete,
    };
    if buf.len() < hash + 3 {
        return PacketParse::Incomplete;
    }
    let consumed = hash + 3;
    let packet = &buf[start + 1..hash];
    if !verify_checksum(packet, &buf[hash + 1..hash + 3]) {
        return PacketParse::Invalid { consumed };
    }
    let data = match std::str::from_utf8(packet) {
        Ok(data) => data.to_string(),
        Err(_) => return PacketParse::Invalid { consumed },
    };
    PacketParse::Complete { data, consumed }
}

/// Build `$<data>#<checksum>` from a plain string.
pub fn format_rsp_packet(data: &str) -> Vec<u8> {
    let ck: u8 = data.bytes().fold(0u8, |a, b| a.wrapping_add(b));
    format!("${}#{:02x}", data, ck).into_bytes()
}

/// Verify the two-character hex checksum `chk_hex` against `data`.
pub fn verify_checksum(data: &[u8], chk_hex: &[u8]) -> bool {
    if chk_hex.len() < 2 {
        return false;
    }
    let hi = match (chk_hex[0] as char).to_digit(16) {
        Some(d) => d as u8,
        None => return false,
    };
    let lo = match (chk_hex[1] as char).to_digit(16) {
        Some(d) => d as u8,
        None => return false,
    };
    let expected: u8 = data.iter().fold(0u8, |a, &b| a.wrapping_add(b));
    (hi << 4 | lo) == expected
}

/// Parse `addr,len` out of the argument portion of an `m` or `M` command.
pub fn parse_addr_len(args: &str) -> Option<(u64, usize)> {
    let mut parts = args.splitn(2, ',');
    let addr = u64::from_str_radix(parts.next()?.trim(), 16).ok()?;
    let len_str = parts.next()?.split(':').next()?; // M has `:data` after len
    let len = usize::from_str_radix(len_str.trim(), 16).ok()?;
    Some((addr, len))
}

/// Parse a `Z/z type,addr,kind` request supported by probe-rs 0.24.
///
/// `Ok(Some(addr))` is a hardware instruction breakpoint. `Ok(None)` is an
/// unsupported breakpoint type and must receive an empty RSP reply.
pub fn parse_hw_breakpoint_addr(args: &str) -> Result<Option<u64>, ()> {
    let mut parts = args.splitn(3, ',');
    let breakpoint_type = parts.next().and_then(|s| s.parse::<u8>().ok()).ok_or(())?;
    if breakpoint_type != 1 {
        return Ok(None);
    }

    let addr = parts
        .next()
        .and_then(|s| u64::from_str_radix(s, 16).ok())
        .ok_or(())?;
    Ok(Some(addr))
}

/// Decode the hex-encoded byte string after `M addr,len:` into bytes.
pub fn decode_hex_bytes(hex: &str) -> Option<Vec<u8>> {
    let bytes = hex.as_bytes();
    if bytes.len() % 2 != 0 {
        return None;
    }
    bytes
        .chunks_exact(2)
        .map(|pair| Some((hex_nibble(pair[0])? << 4) | hex_nibble(pair[1])?))
        .collect()
}

fn hex_nibble(byte: u8) -> Option<u8> {
    match byte {
        b'0'..=b'9' => Some(byte - b'0'),
        b'a'..=b'f' => Some(byte - b'a' + 10),
        b'A'..=b'F' => Some(byte - b'A' + 10),
        _ => None,
    }
}

/// Encode bytes as a lowercase hex string.
pub fn encode_hex_bytes(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{:02x}", b)).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_raw_interrupt_in_stream_order() {
        assert_eq!(
            parse_rsp_packet(b"-\x03"),
            PacketParse::Interrupt { consumed: 2 }
        );
        assert_eq!(
            parse_rsp_packet(b"\x03$g#67"),
            PacketParse::Interrupt { consumed: 1 }
        );
        assert_eq!(
            parse_rsp_packet(b"$g#67\x03"),
            PacketParse::Complete {
                data: "g".to_string(),
                consumed: 5,
            }
        );
        // Ctrl-C inside a packet is payload, not an out-of-band interrupt.
        assert_eq!(
            parse_rsp_packet(b"$\x03#03"),
            PacketParse::Complete {
                data: "\x03".to_string(),
                consumed: 5,
            }
        );
    }

    #[test]
    fn parse_basic_packet() {
        // $g#67
        let buf = b"$g#67";
        let PacketParse::Complete { data, consumed } = parse_rsp_packet(buf) else {
            panic!("expected complete packet");
        };
        assert_eq!(data, "g");
        assert_eq!(consumed, 5);
    }

    #[test]
    fn parse_returns_leading_ack_before_packet() {
        let buf = b"+$g#67";
        assert_eq!(parse_rsp_packet(buf), PacketParse::Ack { consumed: 1 });
        assert_eq!(
            parse_rsp_packet(&buf[1..]),
            PacketParse::Complete {
                data: "g".to_string(),
                consumed: 5,
            }
        );
    }

    #[test]
    fn parse_incomplete_returns_none() {
        assert_eq!(parse_rsp_packet(b"$g#6"), PacketParse::Incomplete);
        assert_eq!(parse_rsp_packet(b"$g"), PacketParse::Incomplete);
        assert_eq!(parse_rsp_packet(b""), PacketParse::Incomplete);
    }

    #[test]
    fn parse_rejects_bad_checksum() {
        assert_eq!(
            parse_rsp_packet(b"$g#00"),
            PacketParse::Invalid { consumed: 5 }
        );
        assert_eq!(
            parse_rsp_packet(b"$g#zz"),
            PacketParse::Invalid { consumed: 5 }
        );
    }

    #[test]
    fn parse_memory_read_packet() {
        let data = "m20000000,4";
        let ck: u8 = data.bytes().fold(0, |a, b: u8| a.wrapping_add(b));
        let pkt = format!("${}#{:02x}", data, ck);
        let PacketParse::Complete { data: out, .. } = parse_rsp_packet(pkt.as_bytes()) else {
            panic!("expected complete packet");
        };
        assert_eq!(out, data);
    }

    #[test]
    fn format_roundtrip() {
        let reply = "deadbeef";
        let pkt = format_rsp_packet(reply);
        let PacketParse::Complete { data: parsed, .. } = parse_rsp_packet(&pkt) else {
            panic!("expected complete packet");
        };
        assert_eq!(parsed, reply);
    }

    #[test]
    fn format_empty_reply() {
        // Empty reply = RSP "not supported"
        let pkt = format_rsp_packet("");
        assert_eq!(pkt, b"$#00");
    }

    #[test]
    fn verify_checksum_ok() {
        let data = b"g";
        // sum('g') = 0x67
        assert!(verify_checksum(data, b"67"));
    }

    #[test]
    fn verify_checksum_bad() {
        assert!(!verify_checksum(b"g", b"00"));
    }

    #[test]
    fn parse_addr_len_basic() {
        let (addr, len) = parse_addr_len("20000000,4").unwrap();
        assert_eq!(addr, 0x20000000);
        assert_eq!(len, 4);
    }

    #[test]
    fn parse_addr_len_with_data_suffix() {
        // M command: "20000000,4:deadbeef"
        let (addr, len) = parse_addr_len("20000000,4:deadbeef").unwrap();
        assert_eq!(addr, 0x20000000);
        assert_eq!(len, 4);
    }

    #[test]
    fn parse_hw_breakpoint_accepts_instruction_breakpoint() {
        assert_eq!(
            parse_hw_breakpoint_addr("1,08000100,2"),
            Ok(Some(0x08000100))
        );
    }

    #[test]
    fn parse_hw_breakpoint_rejects_data_watchpoints_as_unsupported() {
        for breakpoint_type in 2..=4 {
            let args = format!("{breakpoint_type},20000000,4");
            assert_eq!(parse_hw_breakpoint_addr(&args), Ok(None));
        }
    }

    #[test]
    fn parse_hw_breakpoint_rejects_malformed_instruction_breakpoint() {
        assert_eq!(parse_hw_breakpoint_addr("1,not-hex,4"), Err(()));
    }

    #[test]
    fn decode_hex_bytes_basic() {
        assert_eq!(
            decode_hex_bytes("deadbeef").unwrap(),
            &[0xde, 0xad, 0xbe, 0xef]
        );
    }

    #[test]
    fn decode_hex_bytes_odd_returns_none() {
        assert!(decode_hex_bytes("abc").is_none());
    }

    #[test]
    fn decode_hex_bytes_rejects_non_ascii_and_non_hex_without_panicking() {
        for input in ["0€", "a界", "é", "💥", "+a", " a", "0g", "aa bb"] {
            assert_eq!(decode_hex_bytes(input), None, "input: {input}");
        }
        let frame = format_rsp_packet("M20000000,2:0€");
        let PacketParse::Complete { data, .. } = parse_rsp_packet(&frame) else {
            panic!("expected a valid UTF-8 frame");
        };
        assert_eq!(decode_hex_bytes(data.split_once(':').unwrap().1), None);
    }

    #[test]
    fn decode_hex_bytes_preserves_all_byte_values_and_ascii_case() {
        let expected: Vec<u8> = (0..=255).collect();
        let encoded = encode_hex_bytes(&expected);
        assert_eq!(decode_hex_bytes(&encoded), Some(expected.clone()));
        assert_eq!(decode_hex_bytes(&encoded.to_uppercase()), Some(expected));
        assert_eq!(decode_hex_bytes(""), Some(Vec::new()));
    }

    #[test]
    fn encode_hex_bytes_basic() {
        assert_eq!(encode_hex_bytes(&[0xde, 0xad, 0xbe, 0xef]), "deadbeef");
    }
}
