#!/usr/bin/env python3
"""Reversibly redirect rev254 synth 468 to a PS2M jingle for hardware testing.

This edits only a local 2004sp-progressive checkout. It does not commit or push.
"""

from __future__ import annotations

import argparse
import json
import shutil
from pathlib import Path

CANDIDATES = (
    Path("bundled/progressive/engine/src/engine/script/handlers/PlayerOps.ts"),
    Path("engine/src/engine/script/handlers/PlayerOps.ts"),
    Path("src/engine/script/handlers/PlayerOps.ts"),
)

IMPORT_ANCHOR = "import SynthSound from '#/network/game/server/model/SynthSound.js';"
IMPORT_LINE = "import MidiJingle from '#/network/game/server/model/MidiJingle.js';"

HANDLER_OLD = """    [ScriptOpcode.SOUND_SYNTH]: checkedHandler(ActivePlayer, state => {
        const [synth, loops, delay] = state.popInts(3);

        check(synth, NumberNotNull);

        const player = state.activePlayer;
        if (player.lowMemory) {
            return;
        }

        player.write(new SynthSound(synth, loops, delay));
    }),
"""


def find_player_ops(root: Path) -> Path:
    for rel in CANDIDATES:
        path = root / rel
        if path.is_file():
            return path
    matches = list(root.rglob("PlayerOps.ts"))
    for path in matches:
        try:
            text = path.read_text(encoding="utf-8")
        except Exception:
            continue
        if "ScriptOpcode.SOUND_SYNTH" in text and "new SynthSound" in text:
            return path
    raise SystemExit(f"Could not find progressive PlayerOps.ts under {root}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "progressive",
        nargs="?",
        type=Path,
        default=Path("../2004sp-progressive"),
        help="path to local 2004sp-progressive checkout",
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        default=Path("build/diagnostics/anvil_4_ps2m.json"),
    )
    parser.add_argument("--restore", action="store_true")
    args = parser.parse_args()

    target = find_player_ops(args.progressive.resolve())
    backup = target.with_suffix(target.suffix + ".ps2-sfx.bak")

    if args.restore:
        if not backup.is_file():
            raise SystemExit(f"No backup exists: {backup}")
        shutil.copy2(backup, target)
        print(f"Restored: {target}")
        return

    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    synth_id = int(manifest["synth_id"])
    mapped_id = int(manifest["mapped_midi_id"])
    duration_ms = int(manifest["duration_ms"])

    if synth_id != 468 or mapped_id != 20468:
        raise SystemExit(
            f"Unexpected proof mapping synth={synth_id} mapped={mapped_id}"
        )

    text = target.read_text(encoding="utf-8")

    marker = "PS2_SFX_JINGLE_TEST"
    if marker in text:
        print(f"Already patched: {target}")
        print(f"Set PS2_SFX_JINGLE_TEST=1 before starting the server.")
        return

    if IMPORT_LINE not in text:
        if IMPORT_ANCHOR not in text:
            raise SystemExit("Could not find SynthSound import anchor")
        text = text.replace(
            IMPORT_ANCHOR,
            IMPORT_LINE + "\n" + IMPORT_ANCHOR,
            1,
        )

    if HANDLER_OLD not in text:
        raise SystemExit(
            "SOUND_SYNTH handler did not match the expected progressive source; "
            "no changes were written."
        )

    handler_new = f"""    [ScriptOpcode.SOUND_SYNTH]: checkedHandler(ActivePlayer, state => {{
        const [synth, loops, delay] = state.popInts(3);

        check(synth, NumberNotNull);

        const player = state.activePlayer;

        // PS2 hardware proof only. The PS2 client ELF remains completely
        // unchanged: synth 468 is represented by rom/ps2audio/20468.ps2m and
        // enters through the already-proven rev254 MIDI_JINGLE packet path.
        // Disabled by default so normal Java/web clients retain synth packet 25.
        if (process.env.PS2_SFX_JINGLE_TEST === '1' && synth === {synth_id}) {{
            player.write(new MidiJingle({mapped_id}, {duration_ms}));
            return;
        }}

        if (player.lowMemory) {{
            return;
        }}

        player.write(new SynthSound(synth, loops, delay));
    }}),
"""

    if not backup.exists():
        shutil.copy2(target, backup)

    text = text.replace(HANDLER_OLD, handler_new, 1)
    target.write_text(text, encoding="utf-8")

    print(f"Patched:      {target}")
    print(f"Backup:       {backup}")
    print(f"Synth:        {synth_id}")
    print(f"PS2M id:      {mapped_id}")
    print(f"Jingle delay: {duration_ms} ms")
    print()
    print("Enable for this server process with:")
    print("  set PS2_SFX_JINGLE_TEST=1")
    print()
    print("Disable without restoring by clearing the variable:")
    print("  set PS2_SFX_JINGLE_TEST=")
    print()
    print("Restore the source file with:")
    print(f"  py -3 tools\\ps2_audio\\patch_progressive_sfx_test.py "
          f"\"{args.progressive}\" --restore")


if __name__ == "__main__":
    main()
