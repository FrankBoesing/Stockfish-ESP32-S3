from config import ConfigError, load_config


def test_example_config_loads_without_token(tmp_path):
    source = __import__("pathlib").Path(__file__).parents[1] / "config.example.toml"
    target = tmp_path / "config.toml"
    target.write_text(source.read_text(encoding="utf-8"), encoding="utf-8")
    cfg = load_config(target)
    assert cfg.opponent.role in {"CHALLENGE", "ACCEPT"}
    assert cfg.game.variant == "standard"
    assert cfg.lichess.token_env == "LICHESS_TOKEN"


def test_invalid_role_is_rejected(tmp_path):
    target = tmp_path / "bad.toml"
    target.write_text(
        '[lichess]\nusername="A"\n[opponent]\nallowed_username="B"\nrole="BOTH"\n',
        encoding="utf-8",
    )
    try:
        load_config(target)
    except ConfigError as exc:
        assert "CHALLENGE or ACCEPT" in str(exc)
    else:
        raise AssertionError("ConfigError expected")
