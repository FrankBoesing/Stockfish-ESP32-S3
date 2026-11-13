# PlatformIO pre-build hook: fetch the exact Stockfish 19 Smallnet NNUE.
#
# The filename is the first 12 hex characters of the network's SHA-256 digest.
# Keep the binary in the project root because network.cpp embeds it via:
#     INCBIN(EmbeddedNNUE, EvalFileDefaultName);

Import("env")

import hashlib
import os
import urllib.request
from pathlib import Path

NNUE_NAME = "nn-61e7af4bb97d.nnue"
NNUE_SHA256_PREFIX = "61e7af4bb97d"
NNUE_URL = "https://tests.stockfishchess.org/api/nn/" + NNUE_NAME
CHUNK_SIZE = 256 * 1024


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            block = stream.read(CHUNK_SIZE)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


project_dir = Path(env.subst("$PROJECT_DIR"))
nnue_path = project_dir / NNUE_NAME
partial_path = project_dir / (NNUE_NAME + ".download")

if nnue_path.is_file():
    actual = sha256_file(nnue_path)
    if actual.startswith(NNUE_SHA256_PREFIX):
        print("Stockfish NNUE: using verified " + NNUE_NAME)
        print("Stockfish NNUE: SHA-256 " + actual)
    else:
        print("Stockfish NNUE: existing file has the wrong SHA-256; downloading again")
        nnue_path.unlink()
else:
    actual = None

if not nnue_path.is_file():
    request = urllib.request.Request(
        NNUE_URL,
        headers={"User-Agent": "PlatformIO-Stockfish-NNUE-fetch/1.0"}
    )
    digest = hashlib.sha256()
    total = 0

    try:
        print("Stockfish NNUE: downloading " + NNUE_URL)
        with urllib.request.urlopen(request, timeout=120) as response:
            with partial_path.open("wb") as output:
                while True:
                    block = response.read(CHUNK_SIZE)
                    if not block:
                        break
                    output.write(block)
                    digest.update(block)
                    total += len(block)

        actual = digest.hexdigest()
        if not actual.startswith(NNUE_SHA256_PREFIX):
            raise RuntimeError(
                "SHA-256 mismatch for " + NNUE_NAME + ": got " + actual
            )

        os.replace(str(partial_path), str(nnue_path))
        print("Stockfish NNUE: downloaded %u bytes" % total)
        print("Stockfish NNUE: verified SHA-256 " + actual)
    except Exception as exc:
        try:
            if partial_path.exists():
                partial_path.unlink()
        except OSError:
            pass
        raise RuntimeError(
            "Could not obtain the required Stockfish 19 Smallnet file. "
            "Download " + NNUE_URL + " and save it as " + str(nnue_path) +
            ". The file's SHA-256 must start with " + NNUE_SHA256_PREFIX +
            ". Original error: " + str(exc)
        )

