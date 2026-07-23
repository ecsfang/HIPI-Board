#!/usr/bin/env python3
"""
HP-IL Bus Trace Parser
=======================

Parses HP-IL loop traces of the form:

    @CMD [arg] > CMD [arg] > CMD [arg] > CMD [arg] >>> CMD [arg]

The first field (prefixed with '@') is the frame as sent by the
controller. Each following '>'-separated field is the same frame as
re-transmitted by one loop device. The final field, after '>>>', is
what has travelled all the way around the loop and back to the
controller.

This is a best-effort interpretation of the classic HP-IL command
set, cross-checked against the HP 82166 "HP-IL Interface
Specification" (Appendix C: Message Coding). A few edge cases
(secondary/extended addressing, parallel-poll bit fields, the 5x
electronic-instrumentation sub-codes) are simplified or left as
placeholders - extend COMMANDS / ACCESSORY_IDS as needed.
"""

import re
import sys
from dataclasses import dataclass, field


# ---------------------------------------------------------------------------
# Command table - extend this as more mnemonics show up in real traces.
# {arg} is substituted with the frame's argument (address / data byte), if any.
# ---------------------------------------------------------------------------
COMMANDS = {
    # --- Ready messages (RDY / ARG / SOT class) ---
    "RFC": "Ready For Command (loop sync)",
    "ETO": "End of Transmission, OK",
    "ETE": "End of Transmission, Error",
    "NRD": "Not Ready for Data",
    "SDA": "Send Data",
    "SST": "Send Status",
    "SDI": "Send Device ID",
    "SAI": "Send Accessory ID",
    "TCT": "Take Control",

    # --- Addressed / Universal commands (CMD class) ---
    "IFC": "Interface Clear",
    "REN": "Remote Enable",
    "NRE": "Not Remote Enable",
    "LLO": "Local Lockout",
    "GTL": "Go To Local",
    "NOP": "No Operation",
    "DCL": "Device Clear",
    "SDC": "Selected Device Clear",
    "PPD": "Parallel Poll Disable",
    "PPU": "Parallel Poll Unconfigure",
    "GET": "Group Execute Trigger",
    "ELN": "Enable Listener Not-ready",
    "EAR": "Enable Auto Read",
    "AAU": "Auto Address Unconfigure",
    "AAD": "Auto Address Enable, next free address {arg}",
    "NAA": "Next Auto Address",
    "IAA": "Illegal Auto Address",
    "AEP": "Auto Extended Primary",
    "IEP": "Illegal Extended Primary",
    "AES": "Auto Extended Secondary",
    "NES": "Next Extended Secondary",
    "IES": "Illegal Extended Secondary",
    "AMP": "Auto Multiple Primary",
    "NMP": "Next Multiple Primary",
    "IMP": "Illegal Multiple Primary",
    "LPD": "Local Power Down",

    # --- Talk/Listen/Secondary addressing (TAG/LAG/SAG class) ---
    "TAD": "Talk Address {arg}",
    "MTA": "My Talk Address",
    "OTA": "Other Talk Address",
    "UNT": "Untalk",
    "LAD": "Listen Address {arg}",
    "MLA": "My Listen Address",
    "UNL": "Unlisten",
    "SAD": "Secondary Address {arg}",
    "MSA": "My Secondary Address",
    "OSA": "Other Secondary Address",
    "DDL": "Device Dependent Listen {arg}",
    "DDT": "Device Dependent Talk {arg}",

    # --- Data / End (DOE class) ---
    "DAB": "Data Byte 0x{arg}",
    "END": "Data Byte 0x{arg} (last)",
    "IDY": "Identify",
    "EOT": "End of Transmission",
}

END_OF_DATA = {"ETO", "ETE", "EOT"}

# Accessory ID codes returned in response to SAI - full class/type table from
# the HP-IL Interface Specification, Appendix C.4 "Accessory Identification".
# Format: first hex digit = class, second hex digit = type within class.
ACCESSORY_IDS = {
    "00": "Controller (limited)",
    "01": "Controller (instrumentation)",
    "02": "Controller (full, automatic)",
    "03": "Controller (full, partial auto)",
    "0E": "Controller",
    "0F": "Controller (extended)",
    "10": "Mass storage",
    "1E": "Mass storage",
    "1F": "Mass storage (extended)",
    "20": "Printer",
    "21": "Printer",
    "22": "Printer",
    "23": "Printer",
    "24": "Printer",
    "2E": "Printer",
    "2F": "Printer (extended)",
    "30": "Display",
    "3E": "Display",
    "3F": "Display (extended)",
    "40": "Interface (HP-IL/GPIO)",
    "41": "Interface (HP-IL modem)",
    "42": "Interface (HP-IL/RS-232-C)",
    "43": "Interface (HP-IL/HP-IB)",
    "4E": "Interface",
    "4F": "Interface (extended)",
    "5E": "Electronic instrument",
    "5F": "Electronic instrument (extended)",
    "60": "Graphic I/O (HP-GL)",
    "6E": "Graphic I/O",
    "6F": "Graphic I/O (extended)",
    "7E": "Analytical/scientific instrument",
    "7F": "Analytical/scientific instrument (extended)",
    "ED": "General device (EPROM programmer)",
    "EE": "General device",
    "EF": "General device (extended)",
    # 5x (51-57): electronic instrumentation, bit-field encoded (source/switch/
    # measurement) rather than a flat lookup - handle separately if needed.
    # Fx: extended class, usage not currently defined.
}

# Device Dependent Listener/Talker command numbers are defined by each
# device individually. These are specific to the HP 82161A Digital Cassette
# Drive (from its Owner's Manual) and are only applied when the addressed
# device is known - via ACCESSORY_IDS - to be a "Mass storage" device.
CASSETTE_DDL = {
    "00": "Write Buffer 0",
    "01": "Write Buffer 1",
    "02": "Write",
    "03": "Set Byte Pointer",
    "04": "Seek",
    "05": "Format",
    "06": "Partial Write",
    "07": "Rewind",
    "08": "Close Record",
    "09": "Transfer Buffer",
    "0A": "Exchange Buffers",
}
CASSETTE_DDT = {
    "00": "Send Buffer 0",
    "01": "Send Buffer 1",
    "02": "Read",
    "03": "Send Position",
    "04": "Exchange Buffers",
}


def cassette_status(hex_byte):
    """Decode a status byte per the HP 82161A's Status Byte Definition
    table. Returns None for values the manual marks 'Not used' or that
    fall outside the defined ranges."""
    try:
        v = int(hex_byte, 16)
    except (TypeError, ValueError):
        return None
    if 0 <= v <= 15:
        return "Idle"
    if 32 <= v <= 63:
        return "Busy"
    return {
        17: "End Of Tape Error",
        18: "Stall Error",
        19: "End/Stall Error",
        20: "No Tape Error",
        21: "Device Error",
        22: "Device Error",
        23: "New Tape Error",
        24: "Time Out Error",
        25: "Record Number Error",
        26: "Checksum Error",
        28: "Size Error",
    }.get(v)

FRAME_RE = re.compile(r"([A-Z]{2,4})(?:\s+([0-9A-Fa-f]{2}))?")
# Matches right after the leading '@' of an entry: the command as sent.
INITIAL_RE = re.compile(r"^@\s*([A-Z]{2,4})(?:\s+([0-9A-Fa-f]{2}))?")
# The '>>>' marker (or longer) that precedes the looped-back frame.
FINAL_MARK_RE = re.compile(r">{2,}")
# Strips manual annotations like 'X' (an ASCII hint scribbled next to a data
# byte) so they don't get mistaken for extra '>' delimiters.
ANNOTATION_RE = re.compile(r"'[^']'")


def iter_entries(lines):
    """Group raw lines into logical trace entries.

    A real capture isn't always one entry per physical line: the logging
    tool can inject free-form comments ('$$$ Opening tape file: ...',
    'Push 04E') that split a single entry across several lines, and the
    comment itself may contain text that only coincidentally looks like a
    frame. So instead of parsing line by line, every entry - which always
    starts with '@' - is collected up to (but not including) the next
    line that starts with '@', and only then parsed as one block.
    """
    entry_lines, start_lineno = [], None
    for lineno, raw in enumerate(lines, 1):
        if raw.lstrip().startswith("@"):
            if entry_lines:
                yield start_lineno, " ".join(entry_lines)
            entry_lines, start_lineno = [raw.strip()], lineno
        elif entry_lines and raw.strip():
            entry_lines.append(raw.strip())
    if entry_lines:
        yield start_lineno, " ".join(entry_lines)


def extract_frames(block):
    """Pull the initial (controller-sent) and final (looped-back) frame
    out of one entry block, ignoring any per-device hops and free-form
    comments in between."""
    block = ANNOTATION_RE.sub("", block)

    m = INITIAL_RE.match(block)
    if not m:
        return None
    initial = m.group(1), m.group(2)

    marks = list(FINAL_MARK_RE.finditer(block))
    if not marks:
        return None
    tail = block[marks[-1].end():].strip()
    fm = FRAME_RE.match(tail)
    if not fm:
        return None
    final = fm.group(1), fm.group(2)

    return initial, final


def fmt_frame(mnemonic, arg):
    return f"{mnemonic} {arg}" if arg else mnemonic


@dataclass
class BusState:
    talker: str = None
    listener: str = None
    collecting: bool = False
    after_sai: bool = False
    after_sst: bool = False
    data_buffer: list = field(default_factory=list)
    devices: dict = field(default_factory=dict)  # address -> known accessory ID text
    last_device_count: int = None
    direction: str = None  # "listen" (host->device, write) or "talk" (device->host, read)


def ascii_repr(hex_byte):
    try:
        val = int(hex_byte, 16)
        if 32 <= val < 127:
            return f"0x{hex_byte} ('{chr(val)}')"
        return f"0x{hex_byte}"
    except (TypeError, ValueError):
        return hex_byte


def describe(mnemonic, arg, state: BusState):
    """Return a human readable description and update loop state."""
    if mnemonic not in COMMANDS:
        return f"<unknown command '{mnemonic}' - add it to COMMANDS>"

    if mnemonic == "TAD":
        state.talker = arg
        state.direction = "talk"
        dev = state.devices.get(arg)
        return COMMANDS[mnemonic].format(arg=arg or "") + (f" ({dev})" if dev else "")
    elif mnemonic == "LAD":
        state.listener = arg
        state.direction = "listen"
        dev = state.devices.get(arg)
        return COMMANDS[mnemonic].format(arg=arg or "") + (f" ({dev})" if dev else "")
    elif mnemonic == "UNL":
        state.listener = None
    elif mnemonic == "UNT":
        state.talker = None
    elif mnemonic == "DDL":
        dev = state.devices.get(state.listener, "")
        note = ""
        if "Mass storage" in dev and arg and arg.upper() in CASSETTE_DDL:
            note = f" ({CASSETTE_DDL[arg.upper()]})"
        return COMMANDS[mnemonic].format(arg=arg or "") + note
    elif mnemonic == "DDT":
        dev = state.devices.get(state.talker, "")
        note = ""
        if "Mass storage" in dev and arg and arg.upper() in CASSETTE_DDT:
            note = f" ({CASSETTE_DDT[arg.upper()]})"
        return COMMANDS[mnemonic].format(arg=arg or "") + note
    elif mnemonic == "SST":
        state.after_sst = True
    elif mnemonic == "AAD":
        # 'arg' is the next free address, i.e. one past the last device
        # that took part - so (arg - 1) devices are actually on the loop.
        try:
            n = int(arg, 16) - 1
        except (TypeError, ValueError):
            n = None
        note = ""
        if n and n > 0:
            note = f" -> {n} device(s) on loop (addr 1-{n})"
            if state.last_device_count is not None and n != state.last_device_count:
                note += f" [changed from {state.last_device_count}]"
            state.last_device_count = n
        return COMMANDS[mnemonic].format(arg=arg if arg else "") + note
    elif mnemonic == "SAI":
        state.collecting = True
        state.after_sai = True
        state.data_buffer = []
    elif mnemonic == "DAB":
        if state.collecting:
            state.data_buffer.append(arg)
        id_note = ""
        if state.after_sai and arg and arg.upper() in ACCESSORY_IDS:
            id_note = f" -> {ACCESSORY_IDS[arg.upper()]}"
            if state.talker:
                state.devices[state.talker] = ACCESSORY_IDS[arg.upper()]
        elif state.after_sst:
            dev = state.devices.get(state.talker, "")
            if "Mass storage" in dev:
                status = cassette_status(arg)
                if status:
                    id_note = f" -> {status}"
        state.after_sst = False
        if state.direction == "listen" and state.listener:
            dir_note = f" (to listener {state.listener})"
        elif state.direction == "talk" and state.talker:
            dir_note = f" (from talker {state.talker})"
        else:
            dir_note = ""
        return f"Data byte {ascii_repr(arg)}{dir_note}{id_note}"
    elif mnemonic in END_OF_DATA:
        if state.collecting and state.data_buffer:
            joined = "".join(
                chr(int(b, 16)) if b and 32 <= int(b, 16) < 127 else f"\\x{b}"
                for b in state.data_buffer
            )
            state.collecting = False
            state.after_sai = False
            return COMMANDS[mnemonic] + f" - data: \"{joined}\""
        state.collecting = False
        state.after_sai = False

    return COMMANDS[mnemonic].format(arg=arg if arg else "")


MIN_DAB_RUN = 4  # shorter runs aren't worth compacting into a hex block


def flush_dab_run(run, state: BusState):
    """Format a buffered run of consecutive data bytes (same direction).
    Long runs become a compact hex+ASCII block; short ones are printed
    one line per byte. If the run ends with an END frame (the frame that
    marks the last byte of a message), that byte is folded into the
    data/count like any other byte - it IS the last byte, not a separate
    event - and a short trailing marker line replaces its own row so the
    value isn't shown twice."""
    ends_with_end = run[-1][1] == "END"
    end_lineno = run[-1][0] if ends_with_end else None
    data_items = run[:-1] if ends_with_end else run
    values = [arg for _, _, arg, _ in run]  # includes the END byte, if any

    if len(run) < MIN_DAB_RUN:
        lines = [
            f"{lineno:4d}: {mnemonic:4s} {arg:<4}| {desc}"
            for lineno, mnemonic, arg, desc in data_items
        ]
    else:
        start_lineno = run[0][0]
        if state.direction == "listen" and state.listener:
            who = state.listener
            header = f"{start_lineno:4d}: DAB (send to device {who})"
            footer_verb = "written to listener"
        else:
            who = state.talker or "?"
            header = f"{start_lineno:4d}: DAB (reading from device {who})"
            footer_verb = "read from talker"

        lines = [header]
        for i in range(0, len(values), 16):
            chunk = values[i : i + 16]
            hex_part = " ".join(chunk).ljust(16 * 3 - 1)
            ascii_part = "".join(
                chr(int(b, 16)) if b and 32 <= int(b, 16) < 127 else "."
                for b in chunk
            )
            lines.append(f"          {hex_part}   | \"{ascii_part}\"")
        lines.append(f"          Total: {len(values)} bytes {footer_verb} {who}")

    if ends_with_end:
        lines.append(f"{end_lineno:4d}: END {'':<4} | End of data")
    return lines


def analyze(lines):
    state = BusState()
    output = []
    dab_run = []  # buffered (lineno, mnemonic, arg, desc) for a data run

    def flush():
        if dab_run:
            output.extend(flush_dab_run(dab_run, state))
            dab_run.clear()

    for lineno, block in iter_entries(lines):
        result = extract_frames(block)
        if not result:
            flush()
            output.append(f"# line {lineno}: could not parse: {block!r}")
            continue
        (initial_mnemonic, initial_arg), (mnemonic, arg) = result

        # A request frame (e.g. SAI or SST) can be answered with a different
        # frame (e.g. DAB) once it reaches the addressed device - catch that
        # transformation here so describe() knows what kind of byte is
        # coming, even though the entry's own mnemonic is SAI/SST, not DAB.
        if initial_mnemonic == "SAI" and mnemonic != "SAI":
            state.collecting = True
            state.after_sai = True
            state.data_buffer = []
        if initial_mnemonic == "SST" and mnemonic != "SST":
            state.after_sst = True

        unchanged = (initial_mnemonic, initial_arg) == (mnemonic, arg)
        desc = describe(mnemonic, arg, state)

        if mnemonic in ("DAB", "END"):
            # Buffer it. In a read (e.g. after DDT), each entry legitimately
            # shows a different value on the way in vs. out - the controller
            # echoes the byte it received last while the device replies with
            # the next one - so a changed value here is normal, not a
            # transformation worth flagging; only the final value (the byte
            # actually delivered) matters, and that's what goes in the run.
            # END is just the frame that marks the last byte of the message -
            # the byte itself is still data, so it joins the same run - and
            # since it IS the last byte, the run is flushed right away.
            dab_run.append((lineno, mnemonic, arg, desc))
            if mnemonic == "END":
                flush()
            continue

        flush()

        loop_note = ""
        if not unchanged:
            # the frame changed somewhere around the loop (e.g. AAD)
            loop_note = f"   [{fmt_frame(initial_mnemonic, initial_arg)} -> {fmt_frame(mnemonic, arg)}]"

        output.append(
            f"{lineno:4d}: {mnemonic:4s}{(' ' + arg) if arg else '':<4} | {desc}{loop_note}"
        )

    flush()
    return output


def main():
    if len(sys.argv) < 2:
        print("Usage: hpil_parser.py <tracefile>")
        sys.exit(1)

    with open(sys.argv[1], "r") as f:
        lines = f.readlines()

    for line in analyze(lines):
        print(line)


if __name__ == "__main__":
    main()