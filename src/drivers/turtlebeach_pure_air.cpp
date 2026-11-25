// Turtle Beach Pure Air wireless mouse on its 2.4 GHz dongle (VID 10F5 / PID 5012, "Pure Air Dongle").
// Protocol worked out from a USBPcap capture of Swarm II and checked against the app's own reading on
// hardware (47 % and 44 % on two days; Swarm II showed the same numbers).
//   Vendor collection FF00:0001 on interface 1: 64-byte feature report 3, used as a command channel.
//   Host sets feature report 3 = `03 <checksum> <command ...>` (checksum = sum of bytes 2..63, mod 256) and
//   reads feature report 3 back about 24 ms later. The dongle only answers inside the cycle Swarm II runs:
//     1. 03 81 80 00 00 01 ...  poll        -> 03 80 00 00 00 (idle)
//     2. 03 d7 82 0a 00 01 40 00 00 00 00 00 00 0a 00 ...   request A -> 03 82 00 0b 00 40 .. (not used here)
//     3. 03 3e 82 3a 00 01 81 00 ...                        request B -> 03 82 00 0b 00 81 06 01 a1 28 13 <pct> ...
//     4. 03 82 81 00 00 01 ...  poll        -> 03 81 00 00 00 (idle)
//   Sent on their own, A and B only get the idle reply back, so the whole cycle is replayed, verbatim.
//   The battery percent is byte 11 of reply B. Replies can arrive in any slot of the cycle: the mouse also
//   pushes unsolicited lighting events (`03 82 00 0b 00 40 01 0c 01 <r> <g> <b> ..`) and answers nothing
//   while it sleeps, so every reply of the cycle is scanned and the cycle retried a few times.
// Only these four read-style frames are ever sent; no setting or lighting command is written.
// Charging state is not reported: no flag in the replies has been identified as one.
#include "drivers.h"

#include <vector>

namespace {

constexpr USHORT kVid = 0x10F5;
constexpr USHORT kPid = 0x5012;

std::vector<BYTE> Frame(std::initializer_list<BYTE> body) {
    std::vector<BYTE> f(64, 0);
    f[0] = 0x03;
    unsigned sum = 0;
    size_t i = 2;
    for (BYTE b : body) {
        f[i++] = b;
        sum += b;
    }
    f[1] = (BYTE)sum;
    return f;
}

// A reply of request B: 03 82 00 0b 00 81 06 01 a1 28 13 <percent> ..
bool DecodeBattery(const std::vector<BYTE>& r, int& pct) {
    if (r.size() < 12) return false;
    if (r[0] != 0x03 || r[1] != 0x82 || r[3] != 0x0B) return false;
    if (r[5] != 0x81 || r[6] != 0x06 || r[7] != 0x01) return false;
    if (r[11] > 100) return false;
    pct = r[11];
    return true;
}

// Runs the poll cycle once. Returns 1 with the percent, 0 when the dongle answered without a battery
// record (mouse asleep, or a lighting event took the slot), -1 when it could not be queried at all.
int Cycle(HidDev& d, int& pct) {
    const std::vector<BYTE> cycle[] = {
        Frame({ 0x80, 0x00, 0x00, 0x01 }),
        Frame({ 0x82, 0x0A, 0x00, 0x01, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x00 }),
        Frame({ 0x82, 0x3A, 0x00, 0x01, 0x81, 0x00 }),
        Frame({ 0x81, 0x00, 0x00, 0x01 }),
    };
    int found = 0;
    for (const std::vector<BYTE>& req : cycle) {
        if (!d.SetFeature(req)) return -1;
        Sleep(24);
        std::vector<BYTE> r;
        if (!d.GetFeature(0x03, r)) return -1;
        if (DecodeBattery(r, pct)) found = 1;
    }
    return found;
}

}  // namespace

void PollTurtleBeachPureAir(PollContext& ctx) {
    if (ctx.IsClaimed(kVid, kPid)) return;
    auto colls = ctx.Find(kVid, kPid, [](const HidColl& c) {
        return c.page() == 0xFF00 && c.usage() == 0x0001 && c.featLen() >= 64;
    });
    for (const HidColl* c : colls) {
        HidDev d;
        if (!d.OpenFeatureOnly(*c)) continue;
        int pct = -1;
        int state = 0;
        for (int attempt = 0; attempt < 4 && state == 0; ++attempt) {
            state = Cycle(d, pct);
            if (state == 0) Sleep(150);
        }
        if (state < 0) continue;  // could not talk to this collection
        DeviceStatus s;
        s.key = L"turtlebeach:pureair";
        s.name = L"Turtle Beach Pure Air";
        s.kind = DeviceKind::Mouse;
        s.via = L"2.4 GHz dongle";
        s.graceSec = 1800;
        if (state == 1)
            s.percent = pct;
        else
            s.asleep = true;  // dongle present, mouse not answering: keep the last value
        ctx.Add(s, kVid, kPid);
        return;
    }
}
