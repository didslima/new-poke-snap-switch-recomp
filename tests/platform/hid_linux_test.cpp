// SPDX-License-Identifier: MIT
// Stage 6C-B1: isolated unit tests for portable HID/Input core (hid.cpp + input_core.cpp).
// Uses a synthetic Backend and synthetic Cpu structs; never initializes SDL3 or executes guest code.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "hle/hle.h"
#include "hle/hle_config.h"
#include "hle/input_core.h"
#include "hle/svc_common.h"

extern "C" void os_get_stats(void*) {}

static std::atomic<int> g_fail{0}, g_checks{0};
#define CHECK(cond) do { g_checks++; if (!(cond)) { std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

namespace {

struct FakeInputState {
    hle::input::RawInput next;
    float last_rumble_low = -1.0f;
    float last_rumble_high = -1.0f;
    int rumble_calls = 0;
    int poll_calls = 0;
};

class FakeBackend final : public hle::input::Backend {
  public:
    explicit FakeBackend(FakeInputState* st) : st_(st) {}
    const char* name() const override { return "fake"; }
    void poll(hle::input::RawInput& out) override {
        st_->poll_calls++;
        out = st_->next;
    }
    void rumble(float low, float high) override {
        st_->rumble_calls++;
        st_->last_rumble_low = low;
        st_->last_rumble_high = high;
    }
  private:
    FakeInputState* st_;
};

}  // namespace

static void test_hid_registry() {
    const char* const kHidSyms[] = {
        "_ZN2nn3hid14InitializeNpadEv",
        "_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm",
        "_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",
        "_ZN2nn3hid15GetNpadStyleSetERKj",
        "_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj",
        "_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj",
        "_ZN2nn3hid19GetTouchScreenStateILm6EEEvPNS0_16TouchScreenStateIXT_EEE",
        "_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",
        "_ZN2nn3hid21IsSixAxisSensorAtRestERKNS0_19SixAxisSensorHandleE",
        "_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE",
        "_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",
        "_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE",
        "_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE",
    };

    std::unordered_map<std::string, GuestFn> bound;
    std::unordered_map<std::string, bool> is_real;
    struct Ctx {
        std::unordered_map<std::string, GuestFn>* map;
        std::unordered_map<std::string, bool>* real;
    } ctx{&bound, &is_real};

    hle::bind_all([](const char* sym, GuestFn fn, void* u) {
        auto* c = static_cast<Ctx*>(u);
        (*c->map)[sym] = fn;
        (*c->real)[sym] = (hle::find_override(sym) == fn);
    }, &ctx);

    for (const char* sym : kHidSyms) {
        CHECK(hle::find_override(sym) != nullptr);
        CHECK(bound[sym] == hle::find_override(sym));
        CHECK(is_real[sym]);
    }
}

static void test_input_core_and_hid_shims() {
    FakeInputState st;
    st.next.focused = true;
    st.next.pad_connected = true;
    st.next.pad_family = hle::input::kPadFamXbox;

    hle::input::Hub& hub = hle::input::hub();
    hub.set_backend(std::make_unique<FakeBackend>(&st));
    hub.set_mapping(hle::input::Mapping::defaults());
    hub.set_menu_open(false);
    hub.set_window_focus(true);

    // Prevent touching user input.json by ensuring hid sees it already initialized or uses temp config.
    // Calling InitializeNpad triggers ensure_init once; point input_map_path to nonexistent temp path first.
    const std::string orig_input_map = hle::config().input_map_path;
    hle::config().input_map_path = "/tmp/snap_hid_test_nonexistent_input_map.json";
    GuestFn init_npad = hle::find_override("_ZN2nn3hid14InitializeNpadEv");
    CHECK(init_npad != nullptr);
    Cpu c{};
    init_npad(&c);
    hub.set_mapping(hle::input::Mapping::defaults());

    // 1. Buttons, triggers, axes, and radial deadzone (Mapping::defaults: East=A, South=B on Switch).
    st.next.pad.reset();
    st.next.pad.set(hle::input::kPadEast);    // Switch A
    st.next.pad.set(hle::input::kPadLShoulder);
    st.next.lx = 0.05f;   // inside radial 0.15 deadzone -> 0
    st.next.ly = 0.05f;
    st.next.lt = 0.90f;   // ZL
    st.next.rt = 0.10f;   // below trigger threshold
    hub.poll(0);

    auto p = hub.pad();
    CHECK(p.connected);
    CHECK((p.buttons & (1ull << hle::input::kA)) != 0);
    CHECK((p.buttons & (1ull << hle::input::kB)) == 0);
    CHECK((p.buttons & (1ull << hle::input::kL)) != 0);
    CHECK((p.buttons & (1ull << hle::input::kZL)) != 0);
    CHECK((p.buttons & (1ull << hle::input::kZR)) == 0);
    CHECK(p.lx == 0 && p.ly == 0);

    st.next.lx = 0.0f;
    st.next.ly = 0.80f;   // outside radial deadzone -> > 0
    hub.poll(0);
    CHECK(hub.pad().ly > 10000);

    // 2. Swap A/B override (East becomes Switch B).
    const int orig_swap = hle::input::pad_fx().swap_ab.exchange(1);
    hub.poll(0);
    p = hub.pad();
    CHECK((p.buttons & (1ull << hle::input::kB)) != 0);
    CHECK((p.buttons & (1ull << hle::input::kA)) == 0);
    hle::input::pad_fx().swap_ab.store(orig_swap);

    // 3. Button injection via Hub::inject_source.
    hub.inject_source({hle::input::Source::Pad, hle::input::kPadStart}, true);
    hub.poll(0);
    CHECK((hub.pad().buttons & (1ull << hle::input::kPlus)) != 0);
    hub.inject_source({hle::input::Source::Pad, hle::input::kPadStart}, false);

    // 4. Touch via synthetic mouse + GetTouchScreenState<6>.
    st.next.mouse_in_window = true;
    st.next.mouse_buttons = 1;
    st.next.mouse_x = 0.5f;
    st.next.mouse_y = 0.25f;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));   // allow GetTouchScreenState's poll(1ms) to run
    GuestFn get_touch = hle::find_override("_ZN2nn3hid19GetTouchScreenStateILm6EEEvPNS0_16TouchScreenStateIXT_EEE");
    CHECK(get_touch != nullptr);
    uint8_t touch_buf[16 + 6 * 0x28] = {};
    c = Cpu{};
    c.x[0] = reinterpret_cast<uint64_t>(touch_buf);
    get_touch(&c);
    int32_t touch_count = 0;
    std::memcpy(&touch_count, touch_buf + 8, 4);
    int32_t tx = 0, ty = 0;
    std::memcpy(&tx, touch_buf + 16 + 16, 4);
    std::memcpy(&ty, touch_buf + 16 + 20, 4);
    CHECK(touch_count == 1);
    CHECK(tx == 640 && ty == 180);
    st.next.mouse_buttons = 0;

    // 5. Gyro + SixAxis overrides.
    const bool orig_gyro = hle::input::pad_fx().gyro_on.exchange(true);
    st.next.gyro_present = true;
    st.next.gyro_y = 45.0f;
    hub.poll(0);

    GuestFn is_rest = hle::find_override("_ZN2nn3hid21IsSixAxisSensorAtRestERKNS0_19SixAxisSensorHandleE");
    GuestFn get_six = hle::find_override("_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE");
    CHECK(is_rest != nullptr && get_six != nullptr);
    c = Cpu{};
    is_rest(&c);
    CHECK(c.x[0] == 0);

    uint8_t six_buf[0x60 * 2] = {};
    uint32_t six_handle = 1;
    c = Cpu{};
    c.x[0] = reinterpret_cast<uint64_t>(six_buf);
    c.x[1] = 2;
    c.x[2] = reinterpret_cast<uint64_t>(&six_handle);
    get_six(&c);
    float ang_y = 0.0f;
    std::memcpy(&ang_y, six_buf + 0x20, 4);
    CHECK(c.x[0] == 1);
    CHECK(ang_y > 0.1f);
    hle::input::pad_fx().gyro_on.store(orig_gyro);

    // 6. Vibration handles + SendVibrationValue -> synthetic rumble.
    GuestFn get_vib = hle::find_override("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE");
    GuestFn send_vib = hle::find_override("_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE");
    CHECK(get_vib != nullptr && send_vib != nullptr);
    uint32_t vib_handles[2] = {};
    uint32_t npad_id = 0;
    c = Cpu{};
    c.x[0] = reinterpret_cast<uint64_t>(vib_handles);
    c.x[1] = 2;
    c.x[2] = reinterpret_cast<uint64_t>(&npad_id);
    c.x[3] = 1;  // FullKey
    get_vib(&c);
    CHECK(c.x[0] == 2);

    float vib_val[4] = {0.6f, 160.0f, 0.4f, 320.0f};
    c = Cpu{};
    c.x[0] = reinterpret_cast<uint64_t>(&vib_handles[0]);
    c.x[1] = reinterpret_cast<uint64_t>(vib_val);
    send_vib(&c);
    CHECK(st.rumble_calls > 0);
    CHECK(st.last_rumble_low >= 0.59f && st.last_rumble_high >= 0.39f);

    // 7. GetNpadState (valid FullKey ID=0 vs unsupported ID=1) + ShowControllerSupport(nullptr).
    GuestFn get_npad = hle::find_override("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj");
    GuestFn get_style = hle::find_override("_ZN2nn3hid15GetNpadStyleSetERKj");
    GuestFn show_ctrl = hle::find_override("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE");
    CHECK(get_npad != nullptr && get_style != nullptr && show_ctrl != nullptr);

    uint8_t npad_state[0x28] = {};
    c = Cpu{};
    c.x[0] = reinterpret_cast<uint64_t>(npad_state);
    c.x[1] = reinterpret_cast<uint64_t>(&npad_id);
    get_npad(&c);
    uint64_t npad_btns = 0;
    uint32_t npad_attr = 0;
    std::memcpy(&npad_btns, npad_state + 8, 8);
    std::memcpy(&npad_attr, npad_state + 0x20, 4);
    CHECK((npad_btns & (1ull << hle::input::kA)) != 0);
    CHECK(npad_attr == 1);

    uint32_t bad_id = 1;
    c = Cpu{};
    c.x[0] = reinterpret_cast<uint64_t>(&bad_id);
    get_style(&c);
    CHECK(c.x[0] == 0);

    c = Cpu{};
    c.x[0] = reinterpret_cast<uint64_t>(npad_state);
    c.x[1] = reinterpret_cast<uint64_t>(&bad_id);
    get_npad(&c);
    std::memcpy(&npad_btns, npad_state + 8, 8);
    std::memcpy(&npad_attr, npad_state + 0x20, 4);
    CHECK(npad_btns == 0 && npad_attr == 0);

    // Null output pointer to ShowControllerSupport and invalid/zero count to SixAxis must not crash.
    c = Cpu{};
    c.x[0] = 0;
    show_ctrl(&c);
    CHECK(c.x[0] == hle::kSuccess);

    uint32_t bad_six = 0x7fffffffu;
    c = Cpu{};
    c.x[0] = reinterpret_cast<uint64_t>(six_buf);
    c.x[1] = 0;
    c.x[2] = reinterpret_cast<uint64_t>(&bad_six);
    get_six(&c);
    CHECK(c.x[0] == 0);

    // 8. Basic concurrency: repeated poll + read across 2 threads.
    std::atomic<bool> stop{false};
    std::thread reader([&] {
        for (int i = 0; i < 200; ++i) {
            auto ps = hub.pad();
            auto sn = hub.sampling_number();
            (void)ps;
            (void)sn;
        }
        stop.store(true);
    });
    while (!stop.load()) hub.poll(0);
    reader.join();
    CHECK(hub.sampling_number() > 0);

    hle::config().input_map_path = orig_input_map;
}

int main() {
    test_hid_registry();
    test_input_core_and_hid_shims();
    std::printf("hid_linux_test: %d checks, %d failures\n", g_checks.load(), g_fail.load());
    return g_fail.load() ? 1 : 0;
}
