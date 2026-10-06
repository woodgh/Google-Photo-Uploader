#include "phone_model.hpp"

#include <cstdio>

static int g_failed = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failed;                                                  \
    }                                                              \
  } while (0)

static const char kBackingUp[] =
    "Current Notification Manager state:\n"
    "  Notification List:\n"
    "    NotificationRecord(0x0b6b5b7f: pkg=com.google.android.apps.photos user=UserHandle{0} id=1 tag=null importance=2 "
    "key=0|com.google.android.apps.photos|1|null|10123: Notification(channel=backup pri=-1 contentView=null vibrate=null "
    "sound=null defaults=0x0 flags=0x6a color=0xff4285f4 category=progress vis=PRIVATE))\n"
    "      uid=10123 userId=0\n"
    "      extras={\n"
    "        android.title=String (\xEB\xB0\xB1\xEC\x97\x85 \xEC\xA4\x91)\n"
    "        android.text=String (2 left)\n"
    "        android.progress=Integer (1)\n"
    "        android.progressMax=Integer (3)\n"
    "        android.progressIndeterminate=Boolean (false)\n"
    "      }\n"
    "    NotificationRecord(0x0c1: pkg=com.android.systemui user=UserHandle{0} id=2 tag=null importance=3 key=x: "
    "Notification(channel=usb pri=0 flags=0x2 category=sys))\n"
    "      extras={\n"
    "        android.title=String (USB debugging connected)\n"
    "      }\n";

static const char kDone[] =
    "  Notification List:\n"
    "    NotificationRecord(0x0b6b5b7f: pkg=com.google.android.apps.photos user=UserHandle{0} id=3 tag=null importance=2 "
    "key=k: Notification(channel=backup pri=-1 flags=0x10 category=status))\n"
    "      extras={\n"
    "        android.title=String (Backup complete)\n"
    "      }\n";

int main() {
  using namespace gpu;

  const auto devices = ParseDevices("* daemon started successfully\nList of devices attached\nFA6AB0301234\tdevice\nemulator-5554\tunauthorized\n\n");
  CHECK(devices.size() == 2);
  CHECK(devices[0].serial == "FA6AB0301234" && devices[0].state == "device");
  CHECK(devices[1].state == "unauthorized");
  CHECK(ParseDevices("List of devices attached\n\n").empty());

  const auto names = ParseNames("2024_05_01_13_ab12cd34.jpg\r\nIMG_0001.jpg\n\n");
  CHECK(names.size() == 2 && names.count("2024_05_01_13_ab12cd34.jpg"));
  CHECK(ParseNames("ls: /sdcard/DCIM/Camera: No such file or directory\n").empty());

  const auto busy = ParsePhotosNotifications(kBackingUp);
  CHECK(busy.active);
  CHECK(busy.summary == "\xEB\xB0\xB1\xEC\x97\x85 \xEC\xA4\x91 \xC2\xB7 2 left [1/3]");  // systemui 알림은 무시

  const auto done = ParsePhotosNotifications(kDone);
  CHECK(!done.active);
  CHECK(done.summary == "Backup complete");

  const auto none = ParsePhotosNotifications("  Notification List:\n");
  CHECK(!none.active && none.summary.empty() && none.signature.empty());

  QuietTimer timer;
  timer.Reset(0);
  CHECK(!timer.Observe(10, none));      // 알림 없음 → 변동 아님
  CHECK(!timer.Done(100, 180));
  CHECK(timer.Observe(120, busy));      // 백업 시작
  CHECK(!timer.Done(1000, 180));        // 백업 중이면 오래 지나도 완료 아님
  CHECK(timer.Observe(1100, done));     // 백업 끝
  CHECK(!timer.Done(1200, 180));
  CHECK(!timer.Observe(1250, done));
  CHECK(timer.Done(1280, 180));         // 변동 없이 180초
  CHECK(timer.QuietFor(1280) == 180);

  if (g_failed) return 1;
  std::printf("phone model tests passed\n");
  return 0;
}
