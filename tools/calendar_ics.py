"""The next meetings for the adapter's reminder (src/cal.h) from an ICS
calendar: Google Calendar's "secret address in iCal format" (calendar
settings), any other .ics URL, or a local .ics file.

Packet 15 <n> {id, start_in_min, duration_min, flags, title} for the adapter's
control characteristic: the next MAX_EVENTS meetings within HORIZON_H hours,
all-day events and declined / cancelled ones left out. flags: 1 = confirmed
(your own event, or an invitation you accepted), 2 = tentative (not answered
yet, or marked tentative). A reminder dismissed on the adapter's screen comes
back as notification 20 <id>: the meeting is dropped for the rest of the day
(remembered in dismissed.json, so a restart does not bring it back).
"""
import datetime as dt
import hashlib
import json
import struct
import unicodedata
import urllib.request
from pathlib import Path

import icalendar
import recurring_ical_events

REFRESH_S = 300
HORIZON_H = 12
MAX_EVENTS = 3
TITLE_MAX = 24
CONFIRMED, TENTATIVE = 1, 2


def ascii_title(s: str) -> str:
    """The adapter's fonts are ASCII: accents dropped, the rest left out."""
    plain = unicodedata.normalize("NFKD", s).encode("ascii", "ignore").decode()
    return " ".join(plain.split())[:TITLE_MAX]


def localize(d):
    """Aware local datetime; a naive one is taken as local time."""
    return d.astimezone()


class Calendar:
    def __init__(self, source: str, state_dir):
        self.source = source
        self.cal = None
        self.me = None
        self.state = Path(state_dir) / "dismissed.json"
        self.dismissed = {}
        try:
            self.dismissed = json.loads(self.state.read_text())
        except (OSError, ValueError):
            pass

    def refresh(self):
        if "://" in self.source:
            with urllib.request.urlopen(self.source, timeout=30) as r:
                raw = r.read()
        else:
            raw = Path(self.source).read_bytes()
        cal = icalendar.Calendar.from_ical(raw)
        # Google names the primary calendar after the account: that tells us
        # which attendee we are, for the accepted / not answered distinction
        name = str(cal.get("X-WR-CALNAME", ""))
        self.me = name.lower() if "@" in name else None
        self.cal = cal

    def flags(self, ev):
        """CONFIRMED / TENTATIVE, None for a declined invitation."""
        status = str(ev.get("STATUS", "")).upper()
        flags = TENTATIVE if status == "TENTATIVE" else CONFIRMED
        attendees = ev.get("ATTENDEE", [])
        if not isinstance(attendees, list):
            attendees = [attendees]
        for a in attendees:
            if self.me and str(a).lower().replace("mailto:", "") == self.me:
                part = str(a.params.get("PARTSTAT", "ACCEPTED")).upper()
                if part == "DECLINED":
                    return None
                if part in ("NEEDS-ACTION", "TENTATIVE"):
                    flags = TENTATIVE
        return flags

    def events(self, now=None):
        """Upcoming (or running) meetings, soonest first."""
        if self.cal is None:
            return []
        now = now or dt.datetime.now().astimezone()
        found = []
        window = recurring_ical_events.of(self.cal).between(now - dt.timedelta(hours=4),
                                                            now + dt.timedelta(hours=HORIZON_H))
        for ev in window:
            start = ev.get("DTSTART").dt
            end = (ev.get("DTEND") or ev.get("DTSTART")).dt
            if not isinstance(start, dt.datetime):      # all-day
                continue
            start, end = localize(start), localize(end)
            if end <= now or str(ev.get("STATUS", "")).upper() == "CANCELLED":
                continue
            flags = self.flags(ev)
            if flags is None:
                continue
            uid = str(ev.get("UID", ""))
            eid = int(hashlib.sha1(f"{uid}|{start.isoformat()}".encode()).hexdigest()[:4], 16)
            title = ascii_title(str(ev.get("SUMMARY", ""))) or "(no title)"
            found.append({"id": eid, "start": start, "end": end, "title": title, "flags": flags})
        found.sort(key=lambda e: e["start"])
        return found

    def packet(self, now=None) -> bytes:
        now = now or dt.datetime.now().astimezone()
        today = now.date().isoformat()
        out, n = b"", 0
        for e in self.events(now):
            if self.dismissed.get(str(e["id"])) == today:
                continue
            start_in = max(-32768, min(32767, round((e["start"] - now).total_seconds() / 60)))
            dur = max(1, min(65535, round((e["end"] - e["start"]).total_seconds() / 60)))
            title = e["title"].encode("ascii")
            out += struct.pack("<HhHBB", e["id"], start_in, dur, e["flags"], len(title)) + title
            n += 1
            if n == MAX_EVENTS:
                break
        return bytes([0x15, n]) + out

    def describe(self, now=None) -> str:
        now = now or dt.datetime.now().astimezone()
        ev = [e for e in self.events(now) if self.dismissed.get(str(e["id"])) != now.date().isoformat()]
        if not ev:
            return "calendar: no meetings in the next %d h" % HORIZON_H
        e = ev[0]
        mins = round((e["start"] - now).total_seconds() / 60)
        return f"calendar: next {e['start']:%H:%M} {e['title']} ({'in %d min' % mins if mins > 0 else 'now'}" \
               f"{', tentative' if e['flags'] == TENTATIVE else ''}), {len(ev)} within {HORIZON_H} h"

    def dismiss(self, eid: int):
        today = dt.date.today().isoformat()
        self.dismissed = {k: v for k, v in self.dismissed.items() if v == today}
        self.dismissed[str(eid)] = today
        try:
            self.state.parent.mkdir(parents=True, exist_ok=True)
            self.state.write_text(json.dumps(self.dismissed))
        except OSError:
            pass
        print(f"calendar: meeting {eid} dismissed for today")
