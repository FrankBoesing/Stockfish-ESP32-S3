from protocol import MessageKind, format_command, parse_message


def test_parse_ready_and_bestmove():
    ready = parse_message("READY LICHESS-BRIDGE/1 ESP32-S3\r\n")
    best = parse_message("BESTMOVE e7e8q\n")
    assert ready is not None and ready.kind == MessageKind.READY
    assert ready.value == "LICHESS-BRIDGE/1 ESP32-S3"
    assert best is not None and best.kind == MessageKind.BESTMOVE
    assert best.value == "e7e8q"


def test_unknown_and_empty_lines_are_safe():
    assert parse_message("\r\n") is None
    unknown = parse_message("debug: boot complete")
    assert unknown is not None and unknown.kind == MessageKind.UNKNOWN


def test_error_parser():
    error = parse_message("ERROR BAD_FEN invalid position")
    assert error is not None
    assert error.kind == MessageKind.ERROR
    assert error.code == "BAD_FEN"
    assert error.value == "invalid position"


def test_format_command_uses_lf():
    assert format_command("GO", "250") == b"GO 250\n"
