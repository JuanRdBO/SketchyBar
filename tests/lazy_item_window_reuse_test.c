/* Isolated. Real production begin + physical wrappers. No Mach/config/callbacks. */
#include "bar.h"
#include "bar_item.h"
#include "bar_manager.h"
#include "display.h"
#include "event.h"
#include "group.h"
#include "mach.h"
#include "misc/helpers.h"
#include "popup.h"
#include "window.h"

#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int SLSMainConnectionID(void);
extern CFArrayRef SLSCopySpacesForWindows(int cid, int selector, CFArrayRef wids);

int g_connection;
CFTypeRef g_transaction;
int g_space_management_mode;
struct bar_manager g_bar_manager;
struct mach_server g_mach_server;
void *g_workspace_context;
char g_name[256];
char g_config_file[4096];
char g_lock_file[MAXLEN];
bool g_volume_events;
bool g_brightness_events;
int64_t g_disable_capture;
pid_t g_pid;
CGError (*SBSLSTransactionAddPostDecodeAction)(CFTypeRef, void (^)()) = NULL;

static int g_fails;
static uint32_t g_real_did;
static uint32_t g_real_did2;

static void die_timeout(int sig) {
  (void)sig;
  fprintf(stderr, "TEST_TIMEOUT\n");
  _exit(2);
}

static void fail(const char* msg) {
  fprintf(stderr, "FAIL %s\n", msg);
  g_fails++;
}

static void expect_eq(const char* msg, int got, int want) {
  if (got != want) {
    fprintf(stderr, "FAIL %s: got %d want %d\n", msg, got, want);
    g_fails++;
  }
}

static void pump(void) {
  CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
}

static void abort_if_on_display(void) {
  for (uint32_t i = 0; i < g_bar_manager.bar_count; i++) {
    CGPoint origin = g_bar_manager.bars[i]->window.origin;
    if (origin.y > -5000.f) {
      fprintf(stderr, "FAIL on-display bar origin y=%.1f\n", origin.y);
      _exit(1);
    }
  }
}

static void load_symbols(void) {
  if (__builtin_available(macOS 26.0, *)) {
    void* lib = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight",
                       RTLD_LAZY | RTLD_LOCAL);
    if (lib)
      SBSLSTransactionAddPostDecodeAction = dlsym(lib, "SLSTransactionAddPostDecodeAction");
  }
}

static void commit_and_pump(void) {
  windows_unfreeze();
  animator_destroy_display_link(&g_bar_manager.animator);
  abort_if_on_display();
  pump();
}

static void post_physical(enum event_type type) {
  struct event event = { (void*)(intptr_t)g_real_did, type };
  event_post(&event);
  animator_destroy_display_link(&g_bar_manager.animator);
  abort_if_on_display();
  pump();
}

extern void display_test_callback(uint32_t did, CGDisplayChangeSummaryFlags flags);
extern int g_test_physical_passes;
extern int g_test_physical_max_depth;
extern void (*g_test_physical_during)(void);

static void settle(void) {
  animator_destroy_display_link(&g_bar_manager.animator);
  abort_if_on_display();
  pump();
}

static void reset_pass_counts(void) {
  g_test_physical_passes = 0;
  g_test_physical_max_depth = 0;
}

static int g_inject_once;
static void inject_callback_during_pass(void) {
  if (!g_inject_once++) display_test_callback(g_real_did, kCGDisplayMovedFlag);
}

static dispatch_semaphore_t g_offmain_returned;
static void* offmain_callback(void* unused) {
  display_test_callback(g_real_did, kCGDisplayMovedFlag);
  dispatch_semaphore_signal(g_offmain_returned);
  return NULL;
}

static void load_real_dids(void) {
  uint32_t list[2] = { 0 };
  uint32_t count = 0;
  CGGetOnlineDisplayList(2, list, &count);
  if (!count) {
    list[0] = (uint32_t)CGMainDisplayID();
    count = list[0] ? 1 : 0;
  }
  g_real_did = count ? list[0] : 0;
  g_real_did2 = count > 1 ? list[1] : 0;
}

static void stub_displays(uint32_t count, int adid0, int adid1, uint32_t active_adid) {
  memset(g_test_display_dids, 0, sizeof(g_test_display_dids));
  memset(g_test_display_adids, 0, sizeof(g_test_display_adids));
  g_test_display_count = count;
  g_test_display_dids[0] = g_real_did;
  g_test_display_adids[0] = adid0;
  if (count > 1) {
    g_test_display_dids[1] = g_real_did2 ? g_real_did2 : g_real_did;
    g_test_display_adids[1] = adid1;
  }
  g_test_display_active_adid = active_adid;
}

static void begin_offscreen(uint32_t displays) {
  g_bar_manager.displays = displays;
  g_bar_manager.sticky = false;
  g_bar_manager.background.y_offset = -20000;
  windows_freeze();
  bar_manager_begin(&g_bar_manager);
  windows_unfreeze();
  abort_if_on_display();
}

static struct bar_item* add_item(const char* name) {
  struct bar_item* item = bar_manager_create_item(&g_bar_manager);
  bar_item_set_name(item, string_copy((char*)name));
  item->drawing = true;
  item->position = POSITION_LEFT;
  item->updates = false;
  return item;
}

static struct window* slot(struct bar_item* item, uint32_t adid) {
  return bar_item_peek_window(item, adid);
}

static int slot_live(struct bar_item* item, uint32_t adid) {
  struct window* window = slot(item, adid);
  return window && window->id ? 1 : 0;
}

static uint64_t first_space(uint32_t wid) {
  if (!wid) return 0;
  CFArrayRef list = cfarray_of_cfnumbers(&wid, sizeof(uint32_t), 1, kCFNumberSInt32Type);
  CFArrayRef spaces = SLSCopySpacesForWindows(g_connection, 0x7, list);
  CFRelease(list);
  if (!spaces || CFArrayGetCount(spaces) < 1) {
    if (spaces) CFRelease(spaces);
    return 0;
  }
  uint64_t sid = 0;
  CFNumberRef number = CFArrayGetValueAtIndex(spaces, 0);
  CFNumberGetValue(number, kCFNumberSInt64Type, &sid);
  CFRelease(spaces);
  return sid;
}

static void expect_offscreen(const char* msg, struct window* window) {
  if (!window || !window->id) return;
  if (CGPointEqualToPoint(window->origin, g_nirvana)) return;
  if (window->origin.y > -5000.f) {
    fprintf(stderr, "FAIL %s origin y=%.1f\n", msg, window->origin.y);
    g_fails++;
  }
}

static void expect_parent_live(const char* msg, struct window* window) {
  if (!window) {
    fail(msg);
    return;
  }
  if (!window->parent) return;
  if (!window->parent->id) fail(msg);
}

int main(void) {
  alarm(15);
  signal(SIGALRM, die_timeout);

  load_real_dids();
  fprintf(stderr, "physical_dids count=%d did1=%u did2=%u\n",
          (g_real_did ? 1 : 0) + (g_real_did2 && g_real_did2 != g_real_did ? 1 : 0),
          g_real_did, g_real_did2);
  if (!g_real_did) {
    fprintf(stderr, "FAIL no display\n");
    return 1;
  }
  if (!g_real_did2 || g_real_did2 == g_real_did) {
    fprintf(stderr, "FAIL adid-shift requires 2 physical DIDs\n");
    return 1;
  }

  g_connection = SLSMainConnectionID();
  if (!g_connection) {
    fprintf(stderr, "FAIL no SLS connection\n");
    return 1;
  }

  load_symbols();

  stub_displays(2, 1, 2, 1);
  memset(&g_bar_manager, 0, sizeof(g_bar_manager));
  bar_manager_init(&g_bar_manager);
  animator_destroy_display_link(&g_bar_manager.animator);
  begin_offscreen(DISPLAY_ALL_PATTERN);

  struct bar_item* drawn = add_item("drawn");
  struct bar_item* hidden = add_item("hidden");
  hidden->drawing = false;

  struct bar_item* host = add_item("popup_host");
  struct bar_item* child = add_item("popup_child");
  child->position = POSITION_POPUP;
  popup_add_item(&host->popup, child);
  host->popup.drawing = true;
  host->popup.adid = 1;
  host->popup.needs_ordering = true;

  windows_freeze();
  bar_manager_refresh(&g_bar_manager, true);
  if (host->popup.drawing) popup_draw(&host->popup);
  windows_unfreeze();
  abort_if_on_display();

  expect_eq("startup bars", (int)g_bar_manager.bar_count, 2);
  expect_eq("drawn adid1", slot_live(drawn, 1), 1);
  expect_eq("drawn adid2", slot_live(drawn, 2), 1);
  expect_eq("hidden none", slot_live(hidden, 1) + slot_live(hidden, 2), 0);
  expect_eq("popup child adid1", slot_live(child, 1), 1);

  struct window* drawn1 = slot(drawn, 1);
  struct window* drawn2 = slot(drawn, 2);
  struct window* child1 = slot(child, 1);
  uint32_t drawn1_id = drawn1->id;
  uint32_t drawn2_id = drawn2->id;
  uint32_t child1_id = child1->id;
  int32_t drawn1_refc = drawn1->refc;
  uint32_t bar0_id = g_bar_manager.bars[0]->window.id;
  uint32_t bar1_id = g_bar_manager.bars[1]->window.id;
  uint32_t bar0_did = g_bar_manager.bars[0]->did;
  uint32_t bar1_did = g_bar_manager.bars[1]->did;

  drawn1->needs_resize = true;
  window_apply_frame(drawn1, true);
  if (drawn1->refc <= drawn1_refc) fail("apply_frame did not retain");

  g_bar_manager.bars[0]->mouse_over = true;
  g_bar_manager.bars[1]->mouse_over = true;
  g_bar_manager.show_in_fullscreen = true;
  post_physical(DISPLAY_MOVED);
  if (g_bar_manager.bars[0]->mouse_over || g_bar_manager.bars[1]->mouse_over)
    fail("physical left mouse_over");

  expect_eq("physical bars", (int)g_bar_manager.bar_count, 2);
  expect_eq("physical kept bar0 id", (int)g_bar_manager.bars[0]->window.id, (int)bar0_id);
  expect_eq("physical kept bar1 id", (int)g_bar_manager.bars[1]->window.id, (int)bar1_id);
  if (g_bar_manager.bars[0]->did != bar0_did) fail("physical changed bar0 did");
  if (g_bar_manager.bars[1]->did != bar1_did) fail("physical changed bar1 did");
  if (slot(drawn, 1) != drawn1) fail("physical replaced adid1 object");
  if (slot(drawn, 2) != drawn2) fail("physical replaced adid2 object");
  expect_eq("physical same adid1 id", (int)drawn1->id, (int)drawn1_id);
  expect_eq("physical same adid2 id", (int)drawn2->id, (int)drawn2_id);
  expect_eq("physical refc after drain", drawn1->refc, 1);
  if (drawn1->parent && !drawn1->parent->id)
    fail("physical kept dangling parent");
  expect_parent_live("physical parent dangling", drawn1);
  if (slot(child, 1) != child1) fail("physical replaced popup child");
  expect_eq("physical popup child id", (int)child1->id, (int)child1_id);
  if (host->popup.needs_ordering) fail("physical left popup ordering dirty");
  expect_parent_live("popup child parent", child1);

  expect_offscreen("bar0 after physical", &g_bar_manager.bars[0]->window);
  expect_offscreen("bar1 after physical", &g_bar_manager.bars[1]->window);
  expect_offscreen("drawn1 after physical", drawn1);
  expect_offscreen("popup after physical", &host->popup.window);
  if (host->popup.adid != 1) fail("popup adid left first bar");
  if (!g_bar_manager.bars[0]->shown) fail("fullscreen shown skipped");

  uint64_t bar0_sid = first_space(g_bar_manager.bars[0]->window.id);
  uint64_t bar1_sid = first_space(g_bar_manager.bars[1]->window.id);
  uint64_t item_sid = first_space(drawn1->id);
  uint64_t popup_sid = first_space(host->popup.window.id);
  if (bar0_sid && item_sid && bar0_sid != item_sid)
    fail("nonsticky space mismatch");
  if (popup_sid && bar0_sid && popup_sid != bar0_sid)
    fail("popup not on adid1 space");
  if (bar0_sid && bar1_sid && bar0_sid != bar1_sid && popup_sid == bar1_sid)
    fail("popup followed last bar space");

  hidden->drawing = true;
  windows_freeze();
  bar_manager_refresh(&g_bar_manager, true);
  windows_unfreeze();
  expect_eq("reveal hidden", slot_live(hidden, 1), 1);
  uint32_t hidden_id = slot(hidden, 1)->id;
  hidden->drawing = false;
  windows_freeze();
  bar_manager_refresh(&g_bar_manager, true);
  windows_unfreeze();
  expect_eq("hide keeps", slot_live(hidden, 1), 1);
  if (slot(hidden, 1)->id != hidden_id) fail("hide recreated");
  if (!CGPointEqualToPoint(slot(hidden, 1)->origin, g_nirvana))
    fail("hide did not park");

  post_physical(DISPLAY_RESIZED);
  expect_eq("hidden after physical", slot_live(hidden, 1), 1);
  if (slot(hidden, 1)->id != hidden_id) fail("physical recreated hidden");
  if (!CGPointEqualToPoint(slot(hidden, 1)->origin, g_nirvana))
    fail("physical unparked hidden");

  stub_displays(1, 1, 0, 1);
  post_physical(DISPLAY_REMOVED);
  expect_eq("subset bars", (int)g_bar_manager.bar_count, 1);
  expect_eq("subset kept surviving bar id", (int)g_bar_manager.bars[0]->window.id, (int)bar0_id);
  if (g_bar_manager.bars[0]->did != bar0_did) fail("subset changed surviving did");
  if (slot(drawn, 1) != drawn1) fail("subset replaced adid1");
  expect_eq("subset kept adid1 id", (int)drawn1->id, (int)drawn1_id);
  expect_eq("subset retired adid2", slot_live(drawn, 2), 0);
  expect_eq("subset retired popup adid2", slot_live(child, 2), 0);

  stub_displays(2, 1, 2, 1);
  post_physical(DISPLAY_ADDED);
  expect_eq("readd bars", (int)g_bar_manager.bar_count, 2);
  expect_eq("readd kept bar0 id", (int)g_bar_manager.bars[0]->window.id, (int)bar0_id);
  if (g_bar_manager.bars[1]->window.id == bar1_id) fail("readd reused retired bar id");
  if (slot(drawn, 1) != drawn1) fail("readd replaced adid1");
  expect_eq("readd kept adid1", (int)drawn1->id, (int)drawn1_id);
  expect_eq("readd created adid2", slot_live(drawn, 2), 1);
  if (!slot_live(drawn, 2)) fail("readd missing adid2 window");
  else if (slot(drawn, 2)->id == drawn2_id) fail("readd reused retired id");

  uint32_t keep_bar_id = g_bar_manager.bars[1]->window.id;
  uint32_t keep_did = g_bar_manager.bars[1]->did;
  fprintf(stderr, "ADID_SHIFT ran keep_bar=%u keep_did=%u did1=%u did2=%u\n",
          keep_bar_id, keep_did, g_real_did, g_real_did2);
  memset(g_test_display_dids, 0, sizeof(g_test_display_dids));
  memset(g_test_display_adids, 0, sizeof(g_test_display_adids));
  g_test_display_count = 1;
  g_test_display_dids[0] = g_real_did2;
  g_test_display_adids[0] = 1;
  g_test_display_active_adid = 1;
  post_physical(DISPLAY_REMOVED);
  expect_eq("adid-shift bars", (int)g_bar_manager.bar_count, 1);
  expect_eq("adid-shift kept second-did bar", (int)g_bar_manager.bars[0]->window.id, (int)keep_bar_id);
  expect_eq("adid-shift now adid1", (int)g_bar_manager.bars[0]->adid, 1);
  if (g_bar_manager.bars[0]->did != keep_did) fail("adid-shift lost surviving did");
  expect_parent_live("adid-shift drawn1 parent", slot(drawn, 1));
  expect_parent_live("adid-shift popup parent", slot(child, 1));
  if (slot(drawn, 1) && slot(drawn, 1)->parent && slot(drawn, 1)->parent->id
      && slot(drawn, 1)->parent->id != g_bar_manager.bars[0]->window.id)
    fail("adid-shift parent not surviving bar");
  stub_displays(2, 1, 2, 1);
  post_physical(DISPLAY_ADDED);
  expect_eq("adid-shift restored bars", (int)g_bar_manager.bar_count, 2);
  expect_parent_live("adid-shift restored drawn1 parent", slot(drawn, 1));
  expect_parent_live("adid-shift restored drawn2 parent", slot(drawn, 2));
  expect_parent_live("adid-shift restored popup parent", slot(child, 1));

  // Coalescer: real display_handler; physical callbacks share one main-queue pass.
  stub_displays(1, 1, 0, 1);
  reset_pass_counts();
  display_test_callback(g_real_did2, kCGDisplayRemoveFlag);
  display_test_callback(g_real_did, kCGDisplayMovedFlag);
  display_test_callback(g_real_did, kCGDisplayDesktopShapeChangedFlag);
  expect_eq("coalesce no pass before main queue", g_test_physical_passes, 0);
  settle();
  expect_eq("coalesce burst 3 callbacks -> passes", g_test_physical_passes, 1);
  expect_eq("coalesce burst final bar_count", (int)g_bar_manager.bar_count, 1);
  expect_eq("coalesce burst final did", (int)g_bar_manager.bars[0]->did, (int)g_real_did);

  stub_displays(2, 1, 2, 1);
  reset_pass_counts();
  g_inject_once = 0;
  g_test_physical_during = inject_callback_during_pass;
  display_test_callback(g_real_did2, kCGDisplayAddFlag);
  settle();
  settle();
  g_test_physical_during = NULL;
  expect_eq("coalesce callback during pass -> passes", g_test_physical_passes, 2);
  expect_eq("coalesce max depth", g_test_physical_max_depth, 1);
  expect_eq("coalesce follow-up bar_count", (int)g_bar_manager.bar_count, 2);

  reset_pass_counts();
  display_test_callback(g_real_did, kCGDisplayBeginConfigurationFlag);
  settle();
  expect_eq("coalesce begin-only callback ignored", g_test_physical_passes, 0);

  reset_pass_counts();
  g_offmain_returned = dispatch_semaphore_create(0);
  pthread_t offmain;
  pthread_create(&offmain, NULL, offmain_callback, NULL);
  long offmain_blocked = dispatch_semaphore_wait(g_offmain_returned,
                           dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC));
  expect_eq("coalesce off-main callback returned without main", (int)(offmain_blocked != 0), 0);
  if (offmain_blocked) {
    while (dispatch_semaphore_wait(g_offmain_returned, DISPATCH_TIME_NOW)) pump();
  }
  pthread_join(offmain, NULL);
  settle();
  expect_eq("coalesce off-main -> passes", g_test_physical_passes, 1);

  reset_pass_counts();
  display_test_callback(g_real_did, kCGDisplayMovedFlag);
  settle();
  expect_eq("coalesce later callback -> passes", g_test_physical_passes, 1);

  reset_pass_counts();
  struct event display_changed = { NULL, DISPLAY_CHANGED };
  event_post(&display_changed);
  settle();
  expect_eq("DISPLAY_CHANGED not routed through physical pass", g_test_physical_passes, 0);
  fprintf(stderr, "COALESCE ran\n");

  uint32_t before_generic = drawn1->id;
  uint32_t bar0_before_generic = g_bar_manager.bars[0]->window.id;
  bar_manager_display_changed(&g_bar_manager);
  commit_and_pump();
  if (!slot_live(drawn, 1)) fail("generic reset lost window");
  else if (slot(drawn, 1)->id == before_generic)
    fail("generic reset reused item id");
  if (!g_bar_manager.bar_count) fail("generic reset lost bar");
  else if (g_bar_manager.bars[0]->window.id == bar0_before_generic)
    fail("generic reset reused bar id");

  bar_manager_destroy(&g_bar_manager);

  stub_displays(1, 3, 0, 3);
  memset(&g_bar_manager, 0, sizeof(g_bar_manager));
  bar_manager_init(&g_bar_manager);
  animator_destroy_display_link(&g_bar_manager.animator);
  begin_offscreen(DISPLAY_MAIN_PATTERN);
  expect_eq("main bar_count", (int)g_bar_manager.bar_count, 1);
  expect_eq("main adid", (int)g_bar_manager.bars[0]->adid, 3);
  drawn = add_item("main_drawn");
  windows_freeze();
  bar_manager_refresh(&g_bar_manager, true);
  windows_unfreeze();
  expect_eq("main slot1 empty", slot_live(drawn, 1), 0);
  expect_eq("main slot2 empty", slot_live(drawn, 2), 0);
  expect_eq("main slot3 live", slot_live(drawn, 3), 1);
  struct window* main_win = slot(drawn, 3);
  uint32_t main_id = main_win->id;
  uint32_t main_bar_id = g_bar_manager.bars[0]->window.id;
  post_physical(DISPLAY_MOVED);
  expect_eq("main physical bar_count", (int)g_bar_manager.bar_count, 1);
  expect_eq("main physical adid", (int)g_bar_manager.bars[0]->adid, 3);
  expect_eq("main physical bar id", (int)g_bar_manager.bars[0]->window.id, (int)main_bar_id);
  if (slot(drawn, 3) != main_win) fail("main physical replaced object");
  expect_eq("main physical id", (int)main_win->id, (int)main_id);
  expect_eq("main still no slot1", slot_live(drawn, 1), 0);
  expect_eq("main still no slot2", slot_live(drawn, 2), 0);
  expect_offscreen("sparse main bar", &g_bar_manager.bars[0]->window);
  expect_offscreen("sparse main item", main_win);
  bar_manager_destroy(&g_bar_manager);

  if (g_fails) {
    fprintf(stderr, "%d failure(s)\n", g_fails);
    return 1;
  }
  printf("PASS lazy-item-window-reuse\n");
  return 0;
}
