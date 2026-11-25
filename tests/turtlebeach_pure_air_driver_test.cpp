// Simulated Pure Air dongle exercising PollTurtleBeachPureAir. The fake dongle only answers inside the poll
// cycle, like the real one, and the test asserts that nothing but the four known read frames is ever sent.
#include "../src/drivers/drivers.h"

#include <cassert>
#include <cstdio>

namespace {

enum class Mode { Normal, Asleep, EventInBSlot, BadPercent, SetFails, GetFails };
Mode mode = Mode::Normal;
int percent = 47;
int opens = 0, cycles = 0, pendingEvents = 0;
std::vector<BYTE> lastReply;
bool openOk = true;

HidColl MakeColl(USHORT vid = 0x10F5, USHORT pid = 0x5012, USHORT page = 0xFF00, USHORT usage = 0x0001,
                 USHORT featLen = 64) {
    HidColl c;
    c.vid = vid;
    c.pid = pid;
    c.iface = 1;
    c.path = L"test";
    c.caps.UsagePage = page;
    c.caps.Usage = usage;
    c.caps.FeatureReportByteLength = featLen;
    return c;
}

std::vector<BYTE> Idle(BYTE code) { return { 0x03, code, 0x00, 0x00, 0x00 }; }

std::vector<BYTE> ReplyA() {
    return { 0x03, 0x82, 0x00, 0x0B, 0x00, 0x40, 0, 0, 0, 0, 0, 0, 0x0A, 0x00, 0x12, 0x1A };
}

std::vector<BYTE> ReplyB(int pct) {
    return { 0x03, 0x82, 0x00, 0x0B, 0x00, 0x81, 0x06, 0x01, 0xA1, 0x28, 0x13, (BYTE)pct, 0, 0, 0, 0 };
}

std::vector<BYTE> LightingEvent() {
    return { 0x03, 0x82, 0x00, 0x0B, 0x00, 0x40, 0x01, 0x0C, 0x01, 0xFF, 0x00, 0xC3, 0, 0, 0, 0x1A };
}

void Reset() {
    mode = Mode::Normal;
    percent = 47;
    opens = cycles = pendingEvents = 0;
    openOk = true;
    lastReply.clear();
}

}  // namespace

bool HidDev::OpenFeatureOnly(const HidColl& c) {
    ++opens;
    if (!openOk) return false;
    c_ = c;
    h_ = reinterpret_cast<HANDLE>(1);
    return true;
}

void HidDev::Close() { h_ = INVALID_HANDLE_VALUE; }

bool HidDev::SetFeature(const std::vector<BYTE>& f) {
    // Read-only guarantee: 64-byte report 3 with a valid checksum, and one of the four known commands.
    assert(f.size() == 64 && f[0] == 0x03);
    unsigned sum = 0;
    for (size_t i = 2; i < f.size(); ++i) sum += f[i];
    assert(f[1] == (BYTE)sum);
    const BYTE cmd = f[2], arg = f[3];
    if (mode == Mode::SetFails) return false;
    if (cmd == 0x80) {
        ++cycles;
        assert(f[5] == 0x01 && arg == 0x00);
        lastReply = Idle(0x80);
    } else if (cmd == 0x81) {
        assert(f[5] == 0x01 && arg == 0x00);
        lastReply = Idle(0x81);
    } else if (cmd == 0x82 && arg == 0x0A) {
        assert(f[6] == 0x40 && f[13] == 0x0A);
        lastReply = mode == Mode::Asleep ? Idle(0x80) : ReplyA();
    } else if (cmd == 0x82 && arg == 0x3A) {
        assert(f[6] == 0x81 && f[7] == 0x00);
        if (mode == Mode::Asleep)
            lastReply = Idle(0x80);
        else if (mode == Mode::EventInBSlot && pendingEvents > 0)
            --pendingEvents, lastReply = LightingEvent();
        else if (mode == Mode::BadPercent)
            lastReply = ReplyB(101);
        else
            lastReply = ReplyB(percent);
    } else {
        assert(!"unexpected command sent to the dongle");
    }
    return true;
}

bool HidDev::GetFeature(BYTE reportId, std::vector<BYTE>& out) {
    assert(reportId == 0x03);
    if (mode == Mode::GetFails) return false;
    out = lastReply;
    return true;
}

int main() {
    // 1. Normal reply: percent read from byte 11 of reply B, listed once as a mouse.
    for (int pct : { 1, 44, 47, 100 }) {
        Reset();
        percent = pct;
        PollContext ctx;
        ctx.colls = { MakeColl() };
        PollTurtleBeachPureAir(ctx);
        assert(ctx.out.size() == 1);
        assert(ctx.out[0].key == L"turtlebeach:pureair" && ctx.out[0].name == L"Turtle Beach Pure Air");
        assert(ctx.out[0].kind == DeviceKind::Mouse && ctx.out[0].via == L"2.4 GHz dongle");
        assert(ctx.out[0].percent == pct && !ctx.out[0].asleep && !ctx.out[0].charging && ctx.out[0].connected);
        assert(ctx.out[0].graceSec == 1800 && ctx.IsClaimed(0x10F5, 0x5012));
        assert(cycles == 1 && opens == 1);
    }

    // 2. Mouse asleep: dongle answers only idle replies. Listed as asleep (last value kept), retried a few times.
    Reset();
    mode = Mode::Asleep;
    {
        PollContext ctx;
        ctx.colls = { MakeColl() };
        PollTurtleBeachPureAir(ctx);
        assert(ctx.out.size() == 1 && ctx.out[0].asleep && ctx.out[0].percent == -1);
        assert(cycles == 4);
    }

    // 3. A lighting event takes the battery slot once: the next cycle still finds the percent.
    Reset();
    mode = Mode::EventInBSlot;
    pendingEvents = 1;
    percent = 44;
    {
        PollContext ctx;
        ctx.colls = { MakeColl() };
        PollTurtleBeachPureAir(ctx);
        assert(ctx.out.size() == 1 && ctx.out[0].percent == 44 && !ctx.out[0].asleep);
        assert(cycles == 2);
    }

    // 4. An out-of-range percent is never shown.
    Reset();
    mode = Mode::BadPercent;
    {
        PollContext ctx;
        ctx.colls = { MakeColl() };
        PollTurtleBeachPureAir(ctx);
        assert(ctx.out.size() == 1 && ctx.out[0].percent == -1 && ctx.out[0].asleep);
    }

    // 5. Transport failures list nothing.
    for (Mode m : { Mode::SetFails, Mode::GetFails }) {
        Reset();
        mode = m;
        PollContext ctx;
        ctx.colls = { MakeColl() };
        PollTurtleBeachPureAir(ctx);
        assert(ctx.out.empty() && !ctx.IsClaimed(0x10F5, 0x5012));
    }
    Reset();
    openOk = false;
    {
        PollContext ctx;
        ctx.colls = { MakeColl() };
        PollTurtleBeachPureAir(ctx);
        assert(ctx.out.empty());
    }

    // 6. Only the vendor command collection of 10F5:5012 is used, and only if nobody claimed the product.
    const HidColl others[] = {
        MakeColl(0x10F5, 0x5012, 0xFF02, 0x0001),  // the 8-byte interface-2 collection
        MakeColl(0x10F5, 0x5012, 0xFF00, 0x0002),
        MakeColl(0x10F5, 0x5012, 0xFF00, 0x0001, 5),
        MakeColl(0x10F5, 0x2251),                   // Stealth 700 transmitter
        MakeColl(0x046D, 0x5012),
    };
    for (const HidColl& c : others) {
        Reset();
        PollContext ctx;
        ctx.colls = { c };
        PollTurtleBeachPureAir(ctx);
        assert(ctx.out.empty() && opens == 0);
    }
    Reset();
    {
        PollContext ctx;
        ctx.colls = { MakeColl() };
        ctx.Claim(0x10F5, 0x5012);
        PollTurtleBeachPureAir(ctx);
        assert(ctx.out.empty() && opens == 0);
    }

    std::puts("turtlebeach_pure_air_driver_test: ok");
    return 0;
}
