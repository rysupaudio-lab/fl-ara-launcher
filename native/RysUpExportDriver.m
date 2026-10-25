// RysUp export driver: run a DAW's own File > Export from INSIDE a hidden copy of the app, so the export
// needs no window on screen, no screen control and no macOS Automation/Accessibility permission.
//
// Why in-process: macOS only lets one app click another app's menus when the controlling app holds the
// Accessibility permission (and Automation for AppleEvents). Code running inside the DAW itself needs
// neither: it calls the menu item's action, presses dialog buttons through the views' own accessibility
// objects and answers Save panels with a fixed path, all as plain method calls on its own objects.
// Live 11, Cubase 15 and Studio Pro 8 are not built with the hardened runtime (checked 2026-10-20), so the
// `daw-export` launcher can start a separate hidden instance with DYLD_INSERT_LIBRARIES pointing here, the
// same way `tools/logic-bounce` drives Logic. GarageBand is hardened and sandboxed and cannot load it.
//
// The dylib does nothing unless /tmp/rysup-daw-export/<bundle id>/request.json exists when it loads AND names this
// app's bundle id. The request is a recipe:
//   {"bundle_id": "com.ableton.live", "save_url": "/abs/out.wav", "timeout": 1800, "stay_hidden": true,
//    "steps": [ {"wait_window": "Gotaway", "timeout": 300}, {"wait": 8},
//               {"menu": ["File", "Export Audio/Video"]}, {"wait_window": "Export", "timeout": 30},
//               {"dump": "export-dialog"}, {"press": "Export"}, {"wait_file": "/abs/out.wav"},
//               {"quit": true}, {"press": "Don't Save", "optional": true, "timeout": 10} ]}
// Steps (each may carry "timeout" seconds and "optional": true):
//   wait N            sleep N seconds
//   wait_window RE    until a window whose title matches RE (case-insensitive) exists
//   wait_no_modal     until no modal window or sheet is up
//   menu [titles]     walk NSApp.mainMenu by title (case/ellipsis-insensitive prefix) and fire the item
//                     ("async": true fires it from a run-loop block, so a modal it opens can't block)
//   make_key RE       make the hidden window matching RE main and key (Cubase greys Add Track without one)
//   press RE          press the first button-like element whose title/label matches RE, searching the
//                     modal window, then sheets, then the key/main window, then every window; "window": RE
//                     narrows the search
//   set_value {"match": RE, "value": V}   set a text field / popup found like press
//   key "return"|"escape"|"tab"|"space"|<char>  send a key down/up to the target window ("window": RE);
//                     with "mods": ["cmd","shift","alt","ctrl"] it is a shortcut (main menu, then NSApp)
// Request "answer" rules answer prompts the app raises on its own, from launch on; see AnswerAlerts.
//   dump NAME         write dump-NAME.json: menu tree, windows, each window's accessibility tree
//   wait_file PATH    until PATH exists and its size stops changing for 3 s
//   open_document {"path": P, "window": RE, "grace": 10}   done once a window matches RE; if none appears
//                     within grace seconds, asks the app to open P in-process (app delegate, then
//                     NSDocumentController, then an odoc Apple event to itself) and waits for the window;
//                     "open_now": true opens P in-process at once (a warm instance, where no launch opened it)
//   close_document {"menu": [...], "window": RE, "settle": N}   warm mode: close the project, Don't Save
//                     ("window_close": true closes the RE window like its close button instead of a menu)
//   quit              mark NSDocuments clean and close them, then fire the app's own Quit menu item
//                     (falling back to terminate:); presses "Don't Save" on prompts, retries once after
//                     20 s, dumps quit-stuck.json after 45 s. Never force-quits.
// Save panels: while "save_url" is set, every NSSavePanel shown by the app returns OK with that URL and is
// never shown; a folder-only open panel (Cubase's "Set Project Folder") gets "dir_url", default the
// save_url's folder, so it is never answered with a file path. Results: done.json (status, step reached, elapsed, panels answered) and log.txt in the
// request folder. The launcher never force-quits; an app that does not quit is reported, not killed.
//
// Warm mode ("warm": true, with an "id"): the app stays open between jobs so only projects are opened and
// closed. After the steps finish (or fail) the driver runs the request's "close" steps (close_document: cancel
// any dialog left up, fire the app's own File > Close, answer "Don't Save"/"No" and wait until the project's
// window is gone), writes done.json (with "request_id" and "closed"), then polls request.json once a second for
// a request with a new "id" and runs it in this same process (its open_document opens the copy in-process). A
// request {"quit": true} ends the instance with the normal quit, busy or idle; so does "idle_quit" seconds
// (default 1200) without a request, and so does a close that did not complete. warm.json in the request folder
// names the instance (pid, dylib, launched_epoch) and its state (busy/idle/quitting); it is removed at exit.
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <Vision/Vision.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <dlfcn.h>
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <stdlib.h>
#include "RysUpMenuNavigation.h"

// One request folder per app (/tmp/rysup-daw-export/<bundle id>), so Studio Pro, Cubase and Live can export
// at the same time. Set in the constructor.
static NSString *const kRoot = @"/tmp/rysup-daw-export";
static NSString *kDir = @"/tmp/rysup-daw-export";

// Every audio file this app creates is noted in <request dir>/written.txt (open/fopen interposed; dyld applies an
// inserted library's __interpose to every image), so an export that writes to the app's own remembered folder
// (Cubase's Export Audio Mixdown "Path", which a warm instance carries over from the previous job) is still found:
// see "wait_written". Only the path is recorded; the call itself is untouched.
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static char gWrittenLog[1024];
static void NoteWritten(const char *path) {
  if (!path || !gWrittenLog[0]) return;
  size_t n = strlen(path);
  if (n < 5) return;
  const char *ext = path + n - 4;
  if (strcasecmp(ext, ".wav") && strcasecmp(ext, ".aif") && strcasecmp(ext - 1, ".aiff") && strcasecmp(ext, ".caf"))
    return;
  int fd = open(gWrittenLog, O_WRONLY | O_CREAT | O_APPEND, 0644);  // our own image: not interposed
  if (fd < 0) return;
  write(fd, path, n);
  write(fd, "\n", 1);
  close(fd);
}
static int rw_open(const char *path, int flags, ...) {
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
  }
  int fd = open(path, flags, mode);
  if (fd >= 0 && (flags & (O_WRONLY | O_RDWR)) && (flags & (O_CREAT | O_TRUNC))) NoteWritten(path);
  return fd;
}
static FILE *rw_fopen(const char *path, const char *mode) {
  FILE *f = fopen(path, mode);
  if (f && mode && (strchr(mode, 'w') || strchr(mode, 'a'))) NoteWritten(path);
  return f;
}
__attribute__((used)) static const struct { const void *n; const void *o; } kInterposeWritten[]
    __attribute__((section("__DATA,__interpose"))) = {{(const void *)rw_open, (const void *)open},
                                                       {(const void *)rw_fopen, (const void *)fopen}};
static NSDictionary *gRequest;
static NSURL *gSaveURL;
static NSURL *gDirURL;
static NSURL *gOpenURL;  // answer for a file-choosing NSOpenPanel (Cubase File > Import > DAWproject)
static NSMutableArray *gPanels;
static NSMutableArray *gExported;
static NSDictionary *gFound;  // the last find_text hit {x, y, text}  // files found by wait_new_file
static NSDate *gStart;
static BOOL gStayHidden;
// Warm mode (see the header).
static BOOL gWarm;
static BOOL gBusy;
static BOOL gQuitting;      // a warm instance is on its way out: no more steps, no new requests
static BOOL gReleased;      // a person opened a document in this hidden copy: it is theirs now (see ReleaseToUser)
static int gOwnOdoc;        // odoc events this driver sent to itself and has not seen arrive yet
static NSUInteger gPhase;   // 0 = the request's steps, 1 = its close steps
static NSString *gRequestId;
static NSNumber *gClosed;   // warm only: did the close steps complete
static NSDictionary *gResult;  // warm only: the steps' outcome, reported once the close has run
static NSDate *gIdleSince;
static double gIdleQuit = 1200;
static NSString *gDylib;
static double gLaunchedEpoch;

static void Log(NSString *s) {
  NSString *p = [kDir stringByAppendingPathComponent:@"log.txt"];
  NSString *line = [NSString stringWithFormat:@"%@ %@\n", [NSDate date], s];
  NSFileHandle *h = [NSFileHandle fileHandleForWritingAtPath:p];
  if (!h) {
    [line writeToFile:p atomically:NO encoding:NSUTF8StringEncoding error:nil];
    return;
  }
  [h seekToEndOfFile];
  [h writeData:[line dataUsingEncoding:NSUTF8StringEncoding]];
  [h closeFile];
}

static void Finish(NSString *status, NSString *detail, NSUInteger step) {
  NSDictionary *d = @{
    @"status" : status,
    @"detail" : detail ?: @"",
    @"step" : @(step),
    @"elapsed" : @(-[gStart timeIntervalSinceNow]),
    @"pid" : @(getpid()),
    @"bundle_id" : [[NSBundle mainBundle] bundleIdentifier] ?: @"",
    @"panels_answered" : gPanels ?: @[],
    @"exported" : gExported ?: @[],
  };
  if (gWarm) {
    NSMutableDictionary *m = [d mutableCopy];
    m[@"warm"] = @YES;
    m[@"request_id"] = gRequestId ?: @"";
    if (gClosed) m[@"closed"] = gClosed;
    d = m;
  }
  NSData *j = [NSJSONSerialization dataWithJSONObject:d options:NSJSONWritingPrettyPrinted error:nil];
  [j writeToFile:[kDir stringByAppendingPathComponent:@"done.json"] atomically:YES];
  Log([NSString stringWithFormat:@"finish %@ at step %lu: %@", status, (unsigned long)step, detail]);
}

#pragma mark - Matching

static NSString *Norm(NSString *s) {
  s = [s stringByReplacingOccurrencesOfString:@"…" withString:@""];
  s = [s stringByReplacingOccurrencesOfString:@"..." withString:@""];
  return [[s stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]] lowercaseString];
}

static BOOL Matches(NSString *text, NSString *pattern) {
  if (!text.length || !pattern.length) return NO;
  NSRegularExpression *re = [NSRegularExpression regularExpressionWithPattern:pattern
                                                                      options:NSRegularExpressionCaseInsensitive
                                                                        error:nil];
  if (re) return [re firstMatchInString:text options:0 range:NSMakeRange(0, text.length)] != nil;
  return [Norm(text) containsString:Norm(pattern)];
}

// Vision's fast engine (the only one left when the Neural Engine path fails) garbles small UI text ("Outyut Chamr*ls"),
// so a plain-text pattern also matches an observation within a third of its length in edits.
static double TextSimilarity(NSString *text, NSString *pattern) {
  // Approximate substring match (Sellers): the fewest edits between the pattern and ANY part of the text, as a fraction of
  // the pattern. A row read as "El v Outyut Chamr*ls" (a checkbox and arrow glued on, three letters garbled) still
  // matches "Output Channels".
  NSString *x = [[text lowercaseString] stringByReplacingOccurrencesOfString:@" " withString:@""];
  NSString *y = [[pattern lowercaseString] stringByReplacingOccurrencesOfString:@" " withString:@""];
  NSUInteger n = x.length, m = y.length;
  if (!n || !m) return 0;
  NSUInteger prev[m + 1], cur[m + 1];
  for (NSUInteger j = 0; j <= m; j++) prev[j] = j;
  NSUInteger best = m;
  for (NSUInteger i = 1; i <= n; i++) {
    cur[0] = 0;  // a match may start anywhere in the text
    for (NSUInteger j = 1; j <= m; j++) {
      NSUInteger cost = [x characterAtIndex:i - 1] == [y characterAtIndex:j - 1] ? 0 : 1;
      cur[j] = MIN(MIN(prev[j] + 1, cur[j - 1] + 1), prev[j - 1] + cost);
    }
    best = MIN(best, cur[m]);
    memcpy(prev, cur, sizeof prev);
  }
  return 1.0 - (double)best / m;
}

// The one observation that reads as PATTERN: best similarity >= 0.66 and clearly ahead of the runner-up (0.05), so
// "Group Channels" read as "Gn)up Chann•ls" never wins for "Output Channels".
static VNRecognizedTextObservation *BestFuzzy(NSArray<VNRecognizedTextObservation *> *results, NSString *pattern) {
  VNRecognizedTextObservation *best = nil;
  double top = 0, second = 0;
  for (VNRecognizedTextObservation *o in results) {
    NSString *t = [o topCandidates:1].firstObject.string;
    if (!t.length || !pattern.length || [pattern rangeOfCharacterFromSet:[NSCharacterSet characterSetWithCharactersInString:@"\\[](){}.*+?|^$"]].location != NSNotFound) continue;
    double sim = TextSimilarity(t, pattern);
    if (sim > top) { second = top; top = sim; best = o; } else if (sim > second) second = sim;
  }
  return best && top >= 0.66 && top - second >= 0.05 ? best : nil;
}

static BOOL FuzzyMatches(NSString *text, NSString *pattern) {
  if (!text.length || !pattern.length || [pattern rangeOfCharacterFromSet:[NSCharacterSet characterSetWithCharactersInString:@"\\[](){}.*+?|^$"]].location != NSNotFound) return NO;
  return TextSimilarity(text, pattern) >= 0.66;
}

#pragma mark - Save panels

static NSURL *PanelURL(id panel) {
  if ([panel isKindOfClass:[NSOpenPanel class]] && [(NSOpenPanel *)panel canChooseDirectories] &&
      ![(NSOpenPanel *)panel canChooseFiles])
    return gDirURL ?: gSaveURL.URLByDeletingLastPathComponent;
  if (gOpenURL && [panel isKindOfClass:[NSOpenPanel class]] && [(NSOpenPanel *)panel canChooseFiles]) return gOpenURL;
  return gSaveURL;
}


// A panel's accessory view (Logic's Export All Tracks options): every popup, checkbox and text field with the
// label just before it, logged as "panel accessory: ..." and listed in done.json. "panel_options" in the request,
// [{"label": RE, "value": "item title" | 0 | 1 | "text"}], sets matching controls before the panel is answered.
static void PanelControls(NSView *v, NSMutableArray *out, NSString **lastLabel) {
  for (NSView *sub in v.subviews) {
    if ([sub isKindOfClass:[NSButton class]] ||
        ([sub isKindOfClass:[NSTextField class]] && ((NSTextField *)sub).isEditable)) {
      [out addObject:@{@"view" : sub, @"label" : *lastLabel ?: @""}];
    } else if ([sub isKindOfClass:[NSTextField class]]) {
      NSString *t = ((NSTextField *)sub).stringValue;
      if (t.length) *lastLabel = t;
    }
    PanelControls(sub, out, lastLabel);
  }
}

static NSString *ControlDescription(NSView *c, NSString *label) {
  if ([c isKindOfClass:[NSPopUpButton class]]) {
    NSPopUpButton *p = (NSPopUpButton *)c;
    return [NSString stringWithFormat:@"popup [%@] = \"%@\" of %@", label, p.titleOfSelectedItem ?: @"",
                                      [p.itemTitles componentsJoinedByString:@" | "]];
  }
  if ([c isKindOfClass:[NSButton class]])
    return [NSString stringWithFormat:@"button \"%@\" [%@] state %ld", ((NSButton *)c).title, label, (long)((NSButton *)c).state];
  return [NSString stringWithFormat:@"field [%@] = \"%@\"", label, ((NSTextField *)c).stringValue];
}

static NSArray *ApplyPanelOptions(NSSavePanel *panel) {
  NSMutableArray *rows = [NSMutableArray array];
  NSView *acc = panel.accessoryView;
  if (!acc) return rows;
  NSMutableArray *controls = [NSMutableArray array];
  NSString *label = nil;
  PanelControls(acc, controls, &label);
  NSArray *rules = [gRequest[@"panel_options"] isKindOfClass:[NSArray class]] ? gRequest[@"panel_options"] : @[];
  for (NSDictionary *c in controls) {
    NSView *v = c[@"view"];
    NSString *key = [v isKindOfClass:[NSButton class]] && ![v isKindOfClass:[NSPopUpButton class]]
                        ? (((NSButton *)v).title.length ? ((NSButton *)v).title : c[@"label"]) : c[@"label"];
    for (NSDictionary *rule in rules) {
      // "items": RE matched against a popup's item titles (labels in a panel's accessory view often sit beside
      // another control); otherwise "label" against the control's title or the label before it.
      if (rule[@"items"]) {
        if (![v isKindOfClass:[NSPopUpButton class]] ||
            !Matches([((NSPopUpButton *)v).itemTitles componentsJoinedByString:@" | "], rule[@"items"]))
          continue;
      } else if (!Matches(key, rule[@"label"])) continue;
      id val = rule[@"value"];
      if ([v isKindOfClass:[NSPopUpButton class]] && [val isKindOfClass:[NSString class]]) {
        NSPopUpButton *p = (NSPopUpButton *)v;
        NSInteger idx = -1;
        for (NSInteger i = 0; i < p.numberOfItems; i++)
          if (Matches([p itemTitleAtIndex:i], val)) { idx = i; break; }
        if (idx >= 0 && idx != p.indexOfSelectedItem) {
          [p selectItemAtIndex:idx];
          if (p.action) [NSApp sendAction:p.action to:p.target from:p];
        }
      } else if ([v isKindOfClass:[NSButton class]] && [val isKindOfClass:[NSNumber class]]) {
        NSButton *b = (NSButton *)v;
        if (b.state != [val integerValue]) {
          b.state = [val integerValue];
          if (b.action) [NSApp sendAction:b.action to:b.target from:b];
        }
      } else if ([v isKindOfClass:[NSTextField class]] && [val isKindOfClass:[NSString class]]) {
        ((NSTextField *)v).stringValue = val;
      }
    }
    NSString *d = ControlDescription(v, c[@"label"]);
    [rows addObject:d];
    Log([@"panel accessory: " stringByAppendingString:d]);
  }
  return rows;
}

static NSModalResponse AnswerPanel(NSSavePanel *panel) {
  NSArray *accessory = ApplyPanelOptions(panel);
  NSURL *url = PanelURL(panel);
  [gPanels addObject:@{@"class" : NSStringFromClass([panel class]), @"title" : panel.title ?: @"",
                       @"url" : url.path ?: @"", @"accessory" : accessory}];
  Log([NSString stringWithFormat:@"answered %@ with %@", NSStringFromClass([panel class]), url.path]);
  return NSModalResponseOK;
}

static NSModalResponse (*origRunModal)(id, SEL);
static void (*origBeginSheet)(id, SEL, NSWindow *, void (^)(NSModalResponse));
static void (*origBegin)(id, SEL, void (^)(NSModalResponse));
static NSURL *(*origURL)(id, SEL);
static NSArray *(*origURLs)(id, SEL);

static NSModalResponse HookRunModal(id self, SEL _cmd) {
  if (!gSaveURL && !gOpenURL) return origRunModal(self, _cmd);
  return AnswerPanel(self);
}
static void HookBeginSheet(id self, SEL _cmd, NSWindow *w, void (^handler)(NSModalResponse)) {
  if (!gSaveURL && !gOpenURL) return origBeginSheet(self, _cmd, w, handler);
  NSModalResponse r = AnswerPanel(self);
  dispatch_async(dispatch_get_main_queue(), ^{ if (handler) handler(r); });
}
static void HookBegin(id self, SEL _cmd, void (^handler)(NSModalResponse)) {
  if (!gSaveURL && !gOpenURL) return origBegin(self, _cmd, handler);
  NSModalResponse r = AnswerPanel(self);
  dispatch_async(dispatch_get_main_queue(), ^{ if (handler) handler(r); });
}
static NSURL *HookURL(id self, SEL _cmd) { return (gSaveURL || gOpenURL) ? PanelURL(self) : origURL(self, _cmd); }
static NSArray *HookURLs(id self, SEL _cmd) { return (gSaveURL || gOpenURL) ? @[ PanelURL(self) ] : origURLs(self, _cmd); }

static void HookSavePanels(void) {
  // NSSavePanel hands out private subclasses (remote panels); hook the base class and any subclass that
  // overrides one of these methods, so whichever class the app gets is answered.
  struct { SEL sel; IMP imp; IMP *orig; } hooks[] = {
    {@selector(runModal), (IMP)HookRunModal, (IMP *)&origRunModal},
    {@selector(beginSheetModalForWindow:completionHandler:), (IMP)HookBeginSheet, (IMP *)&origBeginSheet},
    {@selector(beginWithCompletionHandler:), (IMP)HookBegin, (IMP *)&origBegin},
    {@selector(URL), (IMP)HookURL, (IMP *)&origURL},
    {@selector(URLs), (IMP)HookURLs, (IMP *)&origURLs},
  };
  Class base = [NSSavePanel class];
  for (size_t i = 0; i < sizeof hooks / sizeof hooks[0]; i++) {
    Method m = class_getInstanceMethod(base, hooks[i].sel);
    if (m) *hooks[i].orig = method_setImplementation(m, hooks[i].imp);
  }
  int n = objc_getClassList(NULL, 0);
  Class *all = (Class *)malloc(sizeof(Class) * (size_t)n);
  n = objc_getClassList(all, n);
  for (int c = 0; c < n; c++) {
    Class k = all[c];
    if (k == base) continue;
    Class s = class_getSuperclass(k);
    while (s && s != base) s = class_getSuperclass(s);
    if (!s) continue;
    for (size_t i = 0; i < sizeof hooks / sizeof hooks[0]; i++) {
      unsigned int count = 0;
      Method *ms = class_copyMethodList(k, &count);
      for (unsigned int j = 0; j < count; j++)
        if (method_getName(ms[j]) == hooks[i].sel) method_setImplementation(ms[j], hooks[i].imp);
      free(ms);
    }
  }
  free(all);
}

#pragma mark - Staying hidden

static void (*origActivate)(id, SEL, BOOL);
static void HookActivate(id self, SEL _cmd, BOOL flag) {
  if (!gStayHidden) origActivate(self, _cmd, flag);
}
static void (*origUnhide)(id, SEL, id);
static void HookUnhide(id self, SEL _cmd, id sender) {
  if (!gStayHidden) origUnhide(self, _cmd, sender);
}

static void HookActivation(void) {
  Method m = class_getInstanceMethod([NSApplication class], @selector(activateIgnoringOtherApps:));
  if (m) origActivate = (void *)method_setImplementation(m, (IMP)HookActivate);
  m = class_getInstanceMethod([NSApplication class], @selector(unhide:));
  if (m) origUnhide = (void *)method_setImplementation(m, (IMP)HookUnhide);
  [[NSNotificationCenter defaultCenter] addObserverForName:NSApplicationDidUnhideNotification
                                                    object:nil
                                                     queue:[NSOperationQueue mainQueue]
                                                usingBlock:^(NSNotification *n) {
                                                  if (gStayHidden) [NSApp hide:nil];
                                                }];
}

// Studio Pro opens the mix/stems folder itself when "Close after export" is on.
// Keeping the app hidden does not hide Finder. Suppress only file-viewer actions
// for this request's export directories; ordinary files and web URLs still open.
static BOOL ExportViewerURL(NSURL *url) {
  if (!gStayHidden || !url.isFileURL) return NO;
  NSString *path = url.URLByStandardizingPath.path;
  for (NSURL *root in @[gDirURL ?: [NSNull null], gSaveURL.URLByDeletingLastPathComponent ?: [NSNull null]]) {
    if (![root isKindOfClass:[NSURL class]]) continue;
    NSString *base = root.URLByStandardizingPath.path;
    if ([path isEqual:base] || [path hasPrefix:[base stringByAppendingString:@"/"]]) return YES;
  }
  return NO;
}
static BOOL (*origWorkspaceOpenURL)(id, SEL, NSURL *);
static BOOL HiddenWorkspaceOpenURL(id self, SEL cmd, NSURL *url) {
  BOOL directory = NO;
  if (ExportViewerURL(url) && [[NSFileManager defaultManager] fileExistsAtPath:url.path isDirectory:&directory] && directory) {
    Log([@"suppressed export-folder Finder open: " stringByAppendingString:url.path]);
    return YES;
  }
  return origWorkspaceOpenURL(self, cmd, url);
}
static BOOL (*origWorkspaceOpenFile)(id, SEL, NSString *);
static BOOL HiddenWorkspaceOpenFile(id self, SEL cmd, NSString *path) {
  BOOL directory = NO;
  if (path.length && ExportViewerURL([NSURL fileURLWithPath:path]) &&
      [[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&directory] && directory) {
    Log([@"suppressed export-folder Finder openFile: " stringByAppendingString:path]);
    return YES;
  }
  return origWorkspaceOpenFile(self, cmd, path);
}
static BOOL (*origWorkspaceSelectFile)(id, SEL, NSString *, NSString *);
static BOOL HiddenWorkspaceSelectFile(id self, SEL cmd, NSString *file, NSString *root) {
  if ((file.length && ExportViewerURL([NSURL fileURLWithPath:file])) ||
      (root.length && ExportViewerURL([NSURL fileURLWithPath:root]))) {
    Log(@"suppressed export-file Finder selection");
    return YES;
  }
  return origWorkspaceSelectFile(self, cmd, file, root);
}
static void (*origWorkspaceReveal)(id, SEL, NSArray *);
static void HiddenWorkspaceReveal(id self, SEL cmd, NSArray *urls) {
  NSMutableArray *remaining = [NSMutableArray array];
  for (NSURL *url in urls) if (!ExportViewerURL(url)) [remaining addObject:url];
  if (remaining.count != urls.count) Log(@"suppressed export-file Finder reveal");
  if (remaining.count) origWorkspaceReveal(self, cmd, remaining);
}
static void HookExportViewer(void) {
  Method m = class_getInstanceMethod([NSWorkspace class], @selector(openURL:));
  if (m) origWorkspaceOpenURL = (void *)method_setImplementation(m, (IMP)HiddenWorkspaceOpenURL);
  m = class_getInstanceMethod([NSWorkspace class], @selector(openFile:));
  if (m) origWorkspaceOpenFile = (void *)method_setImplementation(m, (IMP)HiddenWorkspaceOpenFile);
  m = class_getInstanceMethod([NSWorkspace class], @selector(selectFile:inFileViewerRootedAtPath:));
  if (m) origWorkspaceSelectFile = (void *)method_setImplementation(m, (IMP)HiddenWorkspaceSelectFile);
  m = class_getInstanceMethod([NSWorkspace class], @selector(activateFileViewerSelectingURLs:));
  if (m) origWorkspaceReveal = (void *)method_setImplementation(m, (IMP)HiddenWorkspaceReveal);
}

#pragma mark - Menus

static NSMenuItem *FindItem(NSMenu *menu, NSString *title) {
  [menu update];  // let the app validate and fill dynamic menus
  // "#3" picks the item at that index. Studio Pro titles every top-level menu "NSMenuItem", so its File menu
  // can't be reached by title.
  if (title.length > 1 && [title hasPrefix:@"#"]) {
    NSInteger index = [[title substringFromIndex:1] integerValue];
    if ([[NSString stringWithFormat:@"#%ld", (long)index] isEqualToString:title] && index >= 0 &&
        index < menu.numberOfItems)
      return [menu itemAtIndex:index];
    return nil;
  }
  for (NSMenuItem *it in menu.itemArray)
    if ([Norm(it.title) isEqualToString:Norm(title)]) return it;
  for (NSMenuItem *it in menu.itemArray)
    if (it.title.length && [Norm(it.title) hasPrefix:Norm(title)]) return it;
  return nil;
}

static NSString *FireMenuUnguarded(NSArray *path) {
  NSMenu *menu = NSApp.mainMenu;
  NSMenuItem *item = nil;
  for (NSUInteger i = 0; i < path.count; i++) {
    item = FindItem(menu, path[i]);
    if (!item) return [NSString stringWithFormat:@"no menu item \"%@\"", path[i]];
    if (i + 1 < path.count) {
      if (!item.submenu) return [NSString stringWithFormat:@"\"%@\" has no submenu", path[i]];
      // menuNeedsUpdate: is optional and belongs to the SUBMENU's delegate. Calling it unchecked on the
      // parent's delegate threw "unrecognized selector" inside Live and killed it (File > Export Audio/Video).
      id<NSMenuDelegate> del = item.submenu.delegate;
      if ([del respondsToSelector:@selector(menuNeedsUpdate:)]) [del menuNeedsUpdate:item.submenu];
      menu = item.submenu;
    }
  }
  if (!item.isEnabled) return [NSString stringWithFormat:@"\"%@\" is disabled", item.title];
  NSInteger index = [item.menu indexOfItem:item];
  [item.menu performActionForItemAtIndex:index];
  return nil;
}

// An exception thrown inside the host app is fatal to it (Live's GTerminateHandler), so a step reports it as
// a failed step instead: the driver dumps, writes done.json and quits normally.
static NSString *FireMenu(NSArray *path) {
  @try {
    return FireMenuUnguarded(path);
  } @catch (NSException *e) {
    return [NSString stringWithFormat:@"menu %@ threw %@: %@", path, e.name, e.reason];
  }
}

static NSArray *MenuTree(NSMenu *menu, int depth) {
  NSMutableArray *out = [NSMutableArray array];
  if (depth > 4) return out;
  [menu update];
  for (NSMenuItem *it in menu.itemArray) {
    if (it.isSeparatorItem) continue;
    NSMutableDictionary *d = [@{@"title" : it.title ?: @"", @"enabled" : @(it.isEnabled)} mutableCopy];
    if (it.keyEquivalent.length) d[@"key"] = it.keyEquivalent;
    if (it.submenu) d[@"items"] = MenuTree(it.submenu, depth + 1);
    [out addObject:d];
  }
  return out;
}

#pragma mark - Windows and accessibility elements

static NSArray<NSWindow *> *SearchOrder(NSString *windowRE) {
  NSMutableArray *order = [NSMutableArray array];
  void (^add)(NSWindow *) = ^(NSWindow *w) {
    if (w && ![order containsObject:w] && (!windowRE || Matches(w.title, windowRE))) [order addObject:w];
  };
  add(NSApp.modalWindow);
  for (NSWindow *w in NSApp.windows) add(w.attachedSheet);
  add(NSApp.keyWindow);
  add(NSApp.mainWindow);
  for (NSWindow *w in NSApp.windows) add(w);
  return order;
}

static NSString *Str(id v) { return [v isKindOfClass:[NSString class]] ? v : nil; }

static NSString *Label(id el) {
  NSString *s = nil;
  if ([el respondsToSelector:@selector(accessibilityTitle)]) s = Str([el accessibilityTitle]);
  if (!s.length && [el respondsToSelector:@selector(accessibilityLabel)]) s = Str([el accessibilityLabel]);
  if (!s.length && [el respondsToSelector:@selector(title)]) s = Str([el title]);
  if (!s.length && [el respondsToSelector:@selector(accessibilityValue)]) s = Str([el accessibilityValue]);
  return s ?: @"";
}

static NSArray *Children(id el) {
  NSMutableArray *kids = [NSMutableArray array];
  if ([el respondsToSelector:@selector(accessibilityChildren)]) [kids addObjectsFromArray:[el accessibilityChildren] ?: @[]];
  if ([el isKindOfClass:[NSWindow class]] && [(NSWindow *)el contentView] && ![kids containsObject:[(NSWindow *)el contentView]])
    [kids addObject:[(NSWindow *)el contentView]];
  if ([el isKindOfClass:[NSView class]])
    for (NSView *v in [(NSView *)el subviews])
      if (![kids containsObject:v]) [kids addObject:v];
  return kids;
}

static id FindElement(id root, NSString *re, BOOL wantPress, int depth) {
  if (depth > 40 || !root) return nil;
  NSString *role = [root respondsToSelector:@selector(accessibilityRole)] ? Str([root accessibilityRole]) : nil;
  BOOL pressable = [root isKindOfClass:[NSButton class]] || [role isEqualToString:NSAccessibilityButtonRole] ||
                   [role isEqualToString:NSAccessibilityRadioButtonRole] ||
                   [role isEqualToString:NSAccessibilityCheckBoxRole] ||
                   [role isEqualToString:NSAccessibilityMenuButtonRole];
  BOOL valueish = [root isKindOfClass:[NSControl class]] || [role isEqualToString:NSAccessibilityTextFieldRole] ||
                  [role isEqualToString:NSAccessibilityPopUpButtonRole] ||
                  [role isEqualToString:NSAccessibilityComboBoxRole];
  if ((wantPress ? pressable : valueish) && Matches(Label(root), re)) return root;
  for (id kid in Children(root)) {
    id hit = FindElement(kid, re, wantPress, depth + 1);
    if (hit) return hit;
  }
  return nil;
}

static NSDictionary *Tree(id el, int depth) {
  NSMutableDictionary *d = [NSMutableDictionary dictionary];
  d[@"class"] = NSStringFromClass([el class]);
  if ([el respondsToSelector:@selector(accessibilityRole)] && Str([el accessibilityRole])) d[@"role"] = [el accessibilityRole];
  NSString *label = Label(el);
  if (label.length) d[@"label"] = label;
  if ([el respondsToSelector:@selector(accessibilityValue)]) {
    id v = [el accessibilityValue];
    if ([v isKindOfClass:[NSString class]] || [v isKindOfClass:[NSNumber class]]) d[@"value"] = v;
  }
  if ([el respondsToSelector:@selector(accessibilityFrame)]) d[@"frame"] = NSStringFromRect([el accessibilityFrame]);
  if ([el respondsToSelector:@selector(isAccessibilityEnabled)] && ![el isAccessibilityEnabled]) d[@"enabled"] = @NO;
  if (depth < 30) {
    NSMutableArray *kids = [NSMutableArray array];
    for (id k in Children(el)) [kids addObject:Tree(k, depth + 1)];
    if (kids.count) d[@"children"] = kids;
  }
  return d;
}

static void DumpUnguarded(NSString *name);

static void Dump(NSString *name) {
  @try {  // accessibility getters on custom views can throw; a lost dump must not kill the host app
    DumpUnguarded(name);
  } @catch (NSException *e) {
    Log([NSString stringWithFormat:@"dump %@ threw %@: %@", name, e.name, e.reason]);
  }
}

static void CollectViewClasses(NSView *v, NSMutableDictionary *out, int depth) {
  if (!v || depth > 60) return;
  for (Class k = [v class]; k && k != [NSView class]; k = class_getSuperclass(k)) {
    NSString *name = NSStringFromClass(k);
    if ([name hasPrefix:@"NS"] || [name hasPrefix:@"_NS"] || out[name]) continue;
    unsigned int count = 0;
    Method *ms = class_copyMethodList(k, &count);
    NSMutableArray *sels = [NSMutableArray array];
    for (unsigned int i = 0; i < count && i < 400; i++) [sels addObject:NSStringFromSelector(method_getName(ms[i]))];
    free(ms);
    out[name] = @{@"superclass" : NSStringFromClass(class_getSuperclass(k)) ?: @"",
                  @"methods" : [sels sortedArrayUsingSelector:@selector(compare:)]};
  }
  for (NSView *s in v.subviews) CollectViewClasses(s, out, depth + 1);
}

static void DumpUnguarded(NSString *name) {
  NSMutableArray *wins = [NSMutableArray array];
  // The app's own (non-AppKit) view classes and their methods, to find something addressable in custom UIs
  // that expose nothing to accessibility (Studio Pro's CCL).
  NSMutableDictionary *viewClasses = [NSMutableDictionary dictionary];
  for (NSWindow *w in NSApp.windows) CollectViewClasses(w.contentView.superview ?: w.contentView, viewClasses, 0);
  for (NSWindow *w in NSApp.windows)
    [wins addObject:@{@"title" : w.title ?: @"", @"class" : NSStringFromClass([w class]),
                      @"visible" : @(w.isVisible), @"modal" : @(w == NSApp.modalWindow),
                      @"sheet" : @(w.isSheet), @"frame" : NSStringFromRect(w.frame), @"tree" : Tree(w, 0)}];
  NSDictionary *d = @{@"menu" : MenuTree(NSApp.mainMenu, 0), @"windows" : wins,
                      @"key_window" : NSApp.keyWindow.title ?: @"", @"modal_window" : NSApp.modalWindow.title ?: @"",
                      @"view_classes" : viewClasses};
  NSData *j = [NSJSONSerialization dataWithJSONObject:d options:NSJSONWritingPrettyPrinted error:nil];
  NSString *p = [kDir stringByAppendingPathComponent:[NSString stringWithFormat:@"dump-%@.json", name]];
  [j writeToFile:p atomically:YES];
  Log([NSString stringWithFormat:@"dumped %@ (%lu windows)", p, (unsigned long)wins.count]);
}

static BOOL Press(id el) {
  @try {  // a throw inside the host app would kill it; a refused press is just a failed step
    if ([el isKindOfClass:[NSButton class]]) {
      [(NSButton *)el performClick:nil];
      return YES;
    }
    // Accessibility often surfaces the button's cell (NSAlert's "New (1)", Cubase's Safe Mode "OK"), whose
    // accessibilityPerformPress does nothing: click the button that owns it.
    if ([el isKindOfClass:[NSCell class]] && [[(NSCell *)el controlView] isKindOfClass:[NSButton class]]) {
      [(NSButton *)[(NSCell *)el controlView] performClick:nil];
      return YES;
    }
    if ([el respondsToSelector:@selector(accessibilityPerformPress)] && [el accessibilityPerformPress]) return YES;
  } @catch (NSException *e) {
    Log([NSString stringWithFormat:@"press on %@ threw %@: %@", NSStringFromClass([el class]), e.name, e.reason]);
  }
  return NO;
}

static NSString *SetValue(id el, NSString *value) {
  if ([el isKindOfClass:[NSPopUpButton class]]) {
    NSPopUpButton *p = el;
    for (NSMenuItem *it in p.itemArray)
      if (Matches(it.title, value)) {
        [p selectItem:it];
        [p sendAction:p.action to:p.target];
        return nil;
      }
    return [NSString stringWithFormat:@"popup has no item \"%@\"", value];
  }
  if ([el isKindOfClass:[NSTextField class]]) {
    [(NSTextField *)el setStringValue:value];
    [(NSTextField *)el sendAction:[(NSTextField *)el action] to:[(NSTextField *)el target]];
    return nil;
  }
  if ([el respondsToSelector:@selector(setAccessibilityValue:)]) {
    [el setAccessibilityValue:value];
    return nil;
  }
  return @"element takes no value";
}

// key "e" with "mods": ["cmd", "shift", "alt", "ctrl"]. Letters carry their real ANSI key code, since some
// apps (Live, Studio Pro's CCL) read the key code rather than the characters. A key with modifiers goes
// first to the main menu's key equivalents, then through NSApp like a typed shortcut (Studio Pro's menu
// items are untitled, so Cmd+E is the way to its Export Mixdown); a plain key goes to the target window.
// post: queue the events with -[NSApp postEvent:atStart:] so they arrive through the app's own event loop
// (and its modal session), for windows that ignore a direct -[NSWindow sendEvent:].
// direct: send the events to the window matching "window" itself (its performKeyEquivalent:, then sendEvent:),
// for apps whose menus are drawn in their own window rather than the main menu (FL Studio).
static NSString *SendKeyVia(NSString *key, NSArray *mods, NSString *windowRE, BOOL post, BOOL direct) {
  NSDictionary *named = @{@"return" : @"\r", @"enter" : @"\x03", @"escape" : @"\x1b", @"tab" : @"\t", @"space" : @" "};
  NSDictionary *codes = @{
    @"return" : @36, @"enter" : @76, @"escape" : @53, @"tab" : @48, @"space" : @49,
    @"a" : @0, @"s" : @1, @"d" : @2, @"f" : @3, @"h" : @4, @"g" : @5, @"z" : @6, @"x" : @7, @"c" : @8, @"v" : @9,
    @"b" : @11, @"q" : @12, @"w" : @13, @"e" : @14, @"r" : @15, @"y" : @16, @"t" : @17, @"1" : @18, @"2" : @19,
    @"3" : @20, @"4" : @21, @"6" : @22, @"5" : @23, @"9" : @25, @"7" : @26, @"8" : @28, @"0" : @29, @"o" : @31,
    @"u" : @32, @"i" : @34, @"p" : @35, @"l" : @37, @"j" : @38, @"k" : @40, @"n" : @45, @"m" : @46,
  };
  NSString *lower = key.lowercaseString;
  NSString *chars = named[lower] ?: key;
  unsigned short code = [codes[lower] unsignedShortValue];
  NSEventModifierFlags flags = 0;
  for (NSString *m in [mods isKindOfClass:[NSArray class]] ? mods : @[]) {
    NSString *k = m.lowercaseString;
    if ([k isEqualToString:@"cmd"] || [k isEqualToString:@"command"]) flags |= NSEventModifierFlagCommand;
    else if ([k isEqualToString:@"shift"]) flags |= NSEventModifierFlagShift;
    else if ([k isEqualToString:@"alt"] || [k isEqualToString:@"option"]) flags |= NSEventModifierFlagOption;
    else if ([k isEqualToString:@"ctrl"] || [k isEqualToString:@"control"]) flags |= NSEventModifierFlagControl;
    else return [NSString stringWithFormat:@"unknown modifier \"%@\"", m];
  }
  NSString *typed = (flags & NSEventModifierFlagShift) ? chars.uppercaseString : chars;
  NSWindow *w = SearchOrder(windowRE).firstObject;
  if (!w && (!flags || direct)) return @"no window for key";
  NSEventType types[2] = {NSEventTypeKeyDown, NSEventTypeKeyUp};
  for (int t = 0; t < 2; t++) {
    NSEvent *e = [NSEvent keyEventWithType:types[t] location:NSZeroPoint modifierFlags:flags
                                 timestamp:[[NSProcessInfo processInfo] systemUptime]
                              windowNumber:w.windowNumber context:nil characters:typed
               charactersIgnoringModifiers:chars isARepeat:NO keyCode:code];
    if (direct) {
      // Straight to the window, bypassing the key window and the main menu: apps that draw their own menus
      // (FL Studio) take shortcuts in the window itself, which a hidden app never makes key.
      if (t == 0 && [w performKeyEquivalent:e]) {
        Log([NSString stringWithFormat:@"shortcut %@ %@ taken by window %@", mods, key, w.title]);
        break;
      }
      [w sendEvent:e];
    } else if (post) {
      [NSApp postEvent:e atStart:NO];
    } else if (!flags) {
      [w sendEvent:e];
    } else if (t == 0 && [NSApp.mainMenu performKeyEquivalent:e]) {
      Log([NSString stringWithFormat:@"shortcut %@ %@ taken by the main menu", mods, key]);
      break;
    } else {
      [NSApp sendEvent:e];
    }
  }
  return nil;
}

#pragma mark - Opening the project copy

static BOOL WindowMatching(NSString *re) {
  for (NSWindow *w in NSApp.windows)
    if (Matches(w.title, re)) return YES;
  return NO;
}

// Ask the app to open a file the way Finder would, from inside the process, so no Automation permission is
// involved. Apps that are not NSDocument-based (Studio Pro, Cubase, Live) take documents through their app
// delegate or the odoc Apple event; try those in order and report which route took it.
static NSString *OpenDocumentNow(NSString *path);

// Opens the document from a run-loop block, never inside the step runner's main-queue block: an app whose open
// raises a modal alert (Cubase in a warm instance, 2026-10-21) would run that modal inside our block, and the
// main queue (the answer timer, the next step) would stall until a person clicked it. Returns the route name;
// the open itself happens on the next run-loop pass.
static NSString *OpenDocument(NSString *path) {
  NSString *p = [path copy];
  CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
    @try {
      Log([NSString stringWithFormat:@"opening %@ via %@", p, OpenDocumentNow(p)]);
    } @catch (NSException *e) {
      Log([NSString stringWithFormat:@"open threw %@: %@", e.name, e.reason]);
    }
  });
  CFRunLoopWakeUp(CFRunLoopGetMain());
  return @"run-loop block";
}

static NSString *OpenDocumentNowInner(NSString *path);
static int gOwnOpen;
static NSString *OpenDocumentNow(NSString *path) {
  gOwnOpen++;
  @try {
    return OpenDocumentNowInner(path);
  } @finally {
    gOwnOpen--;
  }
}

static NSString *OpenDocumentNowInner(NSString *path) {
  NSURL *url = [NSURL fileURLWithPath:path];
  id<NSApplicationDelegate> del = NSApp.delegate;
  if ([del respondsToSelector:@selector(application:openURLs:)]) {
    [del application:NSApp openURLs:@[ url ]];
    return @"delegate application:openURLs:";
  }
  if ([del respondsToSelector:@selector(application:openFiles:)]) {
    [del application:NSApp openFiles:@[ path ]];
    return @"delegate application:openFiles:";
  }
  if ([del respondsToSelector:@selector(application:openFile:)] && [del application:NSApp openFile:path])
    return @"delegate application:openFile:";
  NSDocumentController *dc = [NSDocumentController sharedDocumentController];
  NSString *type = [dc typeForContentsOfURL:url error:nil];
  if (type && [dc documentClassForType:type]) {
    [dc openDocumentWithContentsOfURL:url
                              display:YES
                    completionHandler:^(NSDocument *doc, BOOL wasOpen, NSError *e) {
                      if (e) Log([NSString stringWithFormat:@"NSDocumentController open failed: %@", e]);
                    }];
    return @"NSDocumentController";
  }
  // Last resort: the same odoc event Finder sends, addressed to this process itself.
  NSAppleEventDescriptor *ev =
      [NSAppleEventDescriptor appleEventWithEventClass:kCoreEventClass
                                               eventID:kAEOpenDocuments
                                      targetDescriptor:[NSAppleEventDescriptor currentProcessDescriptor]
                                              returnID:kAutoGenerateReturnID
                                         transactionID:kAnyTransactionID];
  NSAppleEventDescriptor *list = [NSAppleEventDescriptor listDescriptor];
  [list insertDescriptor:[NSAppleEventDescriptor descriptorWithFileURL:url] atIndex:1];
  [ev setParamDescriptor:list forKeyword:keyDirectObject];
  NSError *err = nil;
  gOwnOdoc++;
  if (![ev sendEventWithOptions:NSAppleEventSendNoReply timeout:5 error:&err]) {
    gOwnOdoc--;
    Log([NSString stringWithFormat:@"odoc to self failed: %@", err]);
  }
  return @"odoc Apple event to self";
}

#pragma mark - A person's own open

// A warm copy idles hidden between jobs. When the person opens a document in that app (Finder double-click,
// Dock, Open With) macOS hands it to the running copy, which is this hidden one: the window would flash and
// vanish (Studio Pro, Gotaway.song, 2026-10-25). Such an open is the person's, so the copy becomes theirs:
// it shows itself, takes no more jobs and never quits on its own. warm.json goes, so the launcher sees an
// unregistered driver copy (orphan_copy) and passes the slot over instead of sending it work.
static void WarmQuit(NSString *why);
static void ReleaseToUser(NSString *why) {
  if (!gReleased) {
    gReleased = YES;
    Log([NSString stringWithFormat:@"released to the person (%@)%@", why, gBusy ? @" during a job" : @""]);
    if (gWarm) [[NSFileManager defaultManager] removeItemAtPath:[kDir stringByAppendingPathComponent:@"warm.json"] error:nil];
    // released-<pid>.json tells the launcher this copy is the person's: never recycle it, refuse jobs beside it.
    NSDictionary *d = @{@"pid" : @(getpid()), @"why" : why ?: @"", @"epoch" : @([[NSDate date] timeIntervalSince1970])};
    [[NSJSONSerialization dataWithJSONObject:d options:0 error:nil]
        writeToFile:[kDir stringByAppendingPathComponent:[NSString stringWithFormat:@"released-%d.json", getpid()]]
         atomically:YES];
  }
  gStayHidden = NO;
  gSaveURL = gDirURL = gOpenURL = nil;  // no panel of theirs is answered for them
  [NSApp unhide:nil];
  [NSApp activateIgnoringOtherApps:YES];
}

// Every Finder / Open With / drag-onto-Dock open of a running Cocoa app reaches its delegate's open methods (or
// NSDocumentController for document apps); our own opens call those same methods from OpenDocumentNow with
// gOwnOpen set, or send an odoc to ourselves (gOwnOdoc). Anything else is the person's. (An Apple-event handler
// wrapper was tried first: calling Cocoa's generic handler from it crashed Studio Pro, 10-25.)
// The job's own project copy (any open_document path of the current request), e.g. the copy the launch hands over.
static BOOL OwnProject(NSArray *paths) {
  NSMutableSet *own = [NSMutableSet set];
  for (NSDictionary *s in [gRequest[@"steps"] isKindOfClass:[NSArray class]] ? gRequest[@"steps"] : @[]) {
    if (![s isKindOfClass:[NSDictionary class]]) continue;
    id od = s[@"open_document"];
    NSString *p = [od isKindOfClass:[NSDictionary class]] ? od[@"path"] : od;
    if ([p isKindOfClass:[NSString class]]) [own addObject:p.stringByStandardizingPath.stringByResolvingSymlinksInPath];
  }
  if (!paths.count) return NO;
  for (id x in paths) {
    NSString *p = [x isKindOfClass:[NSURL class]] ? [x path] : x;
    if (![p isKindOfClass:[NSString class]] || ![own containsObject:p.stringByStandardizingPath.stringByResolvingSymlinksInPath])
      return NO;
  }
  return YES;
}
static void NoteForeignOpen(NSArray *paths) {
  if (gOwnOpen) return;
  if (gOwnOdoc > 0) { gOwnOdoc--; return; }  // our last-resort odoc to self arriving
  if (OwnProject(paths)) return;
  if (gStayHidden || gWarm) ReleaseToUser([NSString stringWithFormat:@"document opened from outside: %@", paths.firstObject]);
}
static IMP gOrigOpenURLs, gOrigOpenFiles, gOrigOpenFile, gOrigDocOpen;
static void RysOpenURLs(id self, SEL cmd, NSApplication *app, NSArray *urls) {
  NoteForeignOpen(urls);
  ((void (*)(id, SEL, id, id))gOrigOpenURLs)(self, cmd, app, urls);
}
static void RysOpenFiles(id self, SEL cmd, NSApplication *app, NSArray *files) {
  NoteForeignOpen(files);
  ((void (*)(id, SEL, id, id))gOrigOpenFiles)(self, cmd, app, files);
}
static BOOL RysOpenFile(id self, SEL cmd, NSApplication *app, NSString *file) {
  NoteForeignOpen(file ? @[ file ] : @[]);
  return ((BOOL (*)(id, SEL, id, id))gOrigOpenFile)(self, cmd, app, file);
}
static void RysDocOpen(id self, SEL cmd, NSURL *url, BOOL display, id handler) {
  if (display) NoteForeignOpen(url ? @[ url ] : @[]);  // a document app's own internal loads pass display:NO
  ((void (*)(id, SEL, id, BOOL, id))gOrigDocOpen)(self, cmd, url, display, handler);
}

// After launch, once the app has set its delegate.
static void HookForeignOpens(void) {
  Class del = object_getClass(NSApp.delegate);
  Method m;
  if (del && (m = class_getInstanceMethod(del, @selector(application:openURLs:))))
    gOrigOpenURLs = method_setImplementation(m, (IMP)RysOpenURLs);
  if (del && (m = class_getInstanceMethod(del, @selector(application:openFiles:))))
    gOrigOpenFiles = method_setImplementation(m, (IMP)RysOpenFiles);
  if (del && (m = class_getInstanceMethod(del, @selector(application:openFile:))))
    gOrigOpenFile = method_setImplementation(m, (IMP)RysOpenFile);
  m = class_getInstanceMethod([NSDocumentController class], @selector(openDocumentWithContentsOfURL:display:completionHandler:));
  if (m) gOrigDocOpen = method_setImplementation(m, (IMP)RysDocOpen);
}

#pragma mark - Quitting

static NSMenuItem *QuitItem(void) {
  // The application menu is the main menu's first submenu; its Quit item carries Cmd-Q.
  NSMenu *appMenu = NSApp.mainMenu.itemArray.firstObject.submenu;
  [appMenu update];
  for (NSMenuItem *it in appMenu.itemArray)
    if ([it.keyEquivalent isEqualToString:@"q"] &&
        (it.keyEquivalentModifierMask & NSEventModifierFlagDeviceIndependentFlagsMask) == NSEventModifierFlagCommand)
      return it;
  for (NSMenuItem *it in appMenu.itemArray)
    if ([Norm(it.title) hasPrefix:@"quit"]) return it;
  return nil;
}

// A normal quit, the way a person's Cmd-Q goes: fire the app's own Quit item, which apps with their own
// document model (Studio Pro, Cubase) route through their own shutdown. -[NSApp terminate:] alone left
// Studio Pro running. Never a force-quit.
static void RequestQuitNow(NSString *why);
// Runs from a run-loop block, never inside a main-queue block: a quit that raises a "save changes?" modal
// (Cubase's terminate:) would otherwise run that modal inside the driver's own block, and QuitWatch (main
// queue) could never press Don't Save, retry or dump.
static void RequestQuit(NSString *why) {
  CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{ RequestQuitNow(why); });
  CFRunLoopWakeUp(CFRunLoopGetMain());
}

static void RequestQuitNow(NSString *why) {
  NSMenuItem *q = QuitItem();
  if (q && q.isEnabled && q.action != @selector(terminate:)) {
    Log([NSString stringWithFormat:@"quit (%@): app's own \"%@\" item", why, q.title]);
    [q.menu performActionForItemAtIndex:[q.menu indexOfItem:q]];
  } else {
    Log([NSString stringWithFormat:@"quit (%@): -[NSApp terminate:]", why]);
    [NSApp terminate:nil];
  }
}

// While quitting, press "Don't Save" on any prompt: only the test copy is open, so nothing of Jordan's can be
// lost and saving the copy is pointless. Only an explicit don't-save/discard button is ever pressed.
static NSString *const kDontSave = @"^(don.?t save|do not save|discard( changes)?)$";
// Yes/No/Cancel save prompts (Studio Pro's NSAlert "Do you want to save your changes in src?" for an
// unsaved .dawproject import) have no Don't Save button; there "No" is the don't-save answer. It is pressed
// only in a window whose text asks about saving, so a "No" on any other question is never touched.
static NSString *const kSaveQuestion = @"\\bsave\\b.*\\?";

static BOOL PressDontSave(NSString *when);
static NSWindow *WindowTitled(NSString *re);

static void QuitWatch(NSDate *since, BOOL retried, BOOL dumped) {
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
    double t = -[since timeIntervalSinceNow];
    PressDontSave(@"quitting");
    BOOL r = retried, d = dumped;
    if (!r && t > 20) {
      r = YES;
      RequestQuit(@"retry");
    }
    if (!d && t > 45) {
      d = YES;
      Dump(@"quit-stuck");  // shows whatever prompt is holding the quit
    }
    if (t < 600) QuitWatch(since, r, d);
  });
}

// Start a normal quit. A dialog the recipe left open (a probe's Export dialog that has no Cancel button the
// driver can reach, as in Studio Pro) would make the app ignore Quit, so first cancel any modal window or
// sheet that is up NOW, before the quit can raise its own "save changes?" prompt, then quit a moment later.
static BOOL CancelModals(NSString *when) {
  BOOL cancelled = NO;
  for (NSWindow *w in NSApp.windows)
    if (w.attachedSheet) {
      Log([NSString stringWithFormat:@"%@: cancelling sheet on \"%@\"", when, w.title]);
      [w endSheet:w.attachedSheet returnCode:NSModalResponseCancel];
      cancelled = YES;
    }
  if (NSApp.modalWindow) {
    Log([NSString stringWithFormat:@"%@: cancelling modal %@ \"%@\"", when,
                                   NSStringFromClass([NSApp.modalWindow class]), NSApp.modalWindow.title]);
    NSWindow *m = NSApp.modalWindow;
    @try {
      [NSApp abortModal];
      [m orderOut:nil];
    } @catch (NSException *e) {
      Log([NSString stringWithFormat:@"cancelling the modal threw %@: %@", e.name, e.reason]);
    }
    cancelled = YES;
  }
  return cancelled;
}

// Press "Don't Save" (or "No" in a window asking about saving) in the first window that offers it.
static BOOL PressDontSave(NSString *when) {
  for (NSWindow *w in SearchOrder(nil)) {
    id hit = FindElement(w, kDontSave, YES, 0);
    if (!hit && FindElement(w, kSaveQuestion, NO, 0)) hit = FindElement(w, @"^no$", YES, 0);
    if (hit && Press(hit)) {
      Log([NSString stringWithFormat:@"pressed \"%@\" while %@", Label(hit), when]);
      return YES;
    }
  }
  return NO;
}

static void BeginQuit(NSString *why) {
  if (gReleased) {  // the person's copy now: they quit it themselves
    Log([NSString stringWithFormat:@"quit (%@) skipped: released to the person", why]);
    return;
  }
  dispatch_async(dispatch_get_main_queue(), ^{
    BOOL cancelled = CancelModals(@"before quit");
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)((cancelled ? 1.5 : 0) * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
                     RequestQuit(why);
                     QuitWatch([NSDate date], NO, NO);
                   });
  });
}

#pragma mark - Recipe runner

static unsigned long long FileSize(NSString *p) {
  return [[[NSFileManager defaultManager] attributesOfItemAtPath:p error:nil] fileSize];
}

static void RunStep(NSArray *steps, NSUInteger i, NSDate *stepStart, NSMutableDictionary *state);

static void Next(NSArray *steps, NSUInteger i, double delay, NSDate *stepStart, NSMutableDictionary *state) {
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)), dispatch_get_main_queue(),
                 ^{ RunStep(steps, i, stepStart, state); });
}

static void Advance(NSArray *steps, NSUInteger i, NSMutableDictionary *state) {
  [state removeAllObjects];
  Next(steps, i + 1, 0.2, [NSDate date], state);
}

static BOOL HasText(id root, NSString *re, int depth) {
  if (depth > 40 || !root) return NO;
  if (Matches(Label(root), re)) return YES;
  for (id kid in Children(root))
    if (HasText(kid, re, depth + 1)) return YES;
  return NO;
}

static void Pressables(id root, NSMutableArray *out, int depth) {
  if (depth > 40 || !root) return;
  NSString *role = [root respondsToSelector:@selector(accessibilityRole)] ? Str([root accessibilityRole]) : nil;
  if ([root isKindOfClass:[NSButton class]] || [role isEqualToString:NSAccessibilityButtonRole]) [out addObject:root];
  for (id kid in Children(root)) Pressables(kid, out, depth + 1);
}

// Request-level "answer" rules for prompts the app raises on its own, checked twice a second from launch on
// (before and during every step, since some come up before the app even finishes launching). A rule matches a
// modal window or sheet when every condition it names holds:
//   "text": RE   some element's label matches     "title": RE   the window title matches
//   "class": RE  the window's class name matches   "bare": true  the window exposes no button at all
//   "untitled": true  the window has an empty title  "max_step": N  only while the recipe is at step N or earlier
// and then acts with the first of these that applies:
//   "press": RE        press the matching button
//   "press_lone": true press the window's only button (unlabeled ones included)
//   "keys": [k, ...]   type the keys, ONE per tick, and only while the same window is still up; a new window
//                      starts again at the first key (Live's boxes: "n" first, so a crash-recovery box is never
//                      answered with Return, which means Yes there)
static __weak NSWindow *gKeysWindow;
static NSUInteger gKeysNext;
static NSUInteger gStepIndex;  // the recipe step running now (0 = opening the document)

static NSString *Click(NSDictionary *c);

// "fail": TEXT on an answer rule: the prompt means the job cannot go on (Cubase: "... was created with Cubase
// Version X. This program version cannot load it."). It is answered as the rule says, then the running step
// fails at once with TEXT instead of waiting out its timeout.
static NSString *gAnswerFail;

static BOOL AnswerWindow(NSWindow *w, NSDictionary *rule) {
  if (rule[@"max_step"] && gStepIndex > [rule[@"max_step"] unsignedIntegerValue]) return NO;
  if ([rule[@"untitled"] boolValue] && w.title.length) return NO;
  if (rule[@"title"] && !Matches(w.title, rule[@"title"])) return NO;
  if (rule[@"class"] && !Matches(NSStringFromClass([w class]), rule[@"class"])) return NO;
  if (rule[@"text"] && !HasText(w, rule[@"text"], 0)) return NO;
  // "size": [w, h] (points, +-2): for prompts that expose no text (Studio Pro's CCL alerts), tell them apart by
  // their content size.
  if ([rule[@"size"] isKindOfClass:[NSArray class]] && [rule[@"size"] count] == 2) {
    NSSize cs = w.contentLayoutRect.size;
    if (fabs(cs.width - [rule[@"size"][0] doubleValue]) > 2 || fabs(cs.height - [rule[@"size"][1] doubleValue]) > 2)
      return NO;
  }
  NSMutableArray *buttons = [NSMutableArray array];
  Pressables(w, buttons, 0);
  if ([rule[@"bare"] boolValue] && buttons.count) return NO;
  if ([rule[@"fail"] isKindOfClass:[NSString class]] && !gAnswerFail) {
    gAnswerFail = rule[@"fail"];
    Log([NSString stringWithFormat:@"prompt \"%@\" fails the job: %@", rule[@"name"] ?: @"", gAnswerFail]);
  }
  NSString *name = rule[@"name"] ?: rule[@"text"] ?: rule[@"title"] ?: NSStringFromClass([w class]);
  if (rule[@"press"]) {
    id hit = FindElement(w, rule[@"press"], YES, 0);
    if (hit && Press(hit)) {
      Log([NSString stringWithFormat:@"answered \"%@\" with \"%@\"", name, Label(hit)]);
      return YES;
    }
  }
  if ([rule[@"press_lone"] boolValue] && buttons.count == 1 && Press(buttons[0])) {
    Log([NSString stringWithFormat:@"answered \"%@\" with its only button", name]);
    return YES;
  }
  NSArray *keys = rule[@"keys"];
  if (gKeysWindow != w) {
    gKeysWindow = w;
    gKeysNext = 0;
  }
  if ([keys isKindOfClass:[NSArray class]] && gKeysNext < keys.count) {
    NSString *k = keys[gKeysNext++];
    SendKeyVia(k, nil, nil, [rule[@"post"] boolValue], NO);
    Log([NSString stringWithFormat:@"answered \"%@\": key %@ (%lu/%lu)", name, k, (unsigned long)gKeysNext,
                                   (unsigned long)keys.count]);
    return YES;
  }
  // "click": {"x", "y", "direct"}: one in-process click at (x, y) points from the window content's top-left,
  // once per window, for prompts whose buttons are drawn by the app and reachable by no accessibility element
  // (FL Studio's "Confirm" box on quit: Yes / No / Cancel at x 68 / 138 / 208, y 154 in a 252x182 window).
  if ([rule[@"click"] isKindOfClass:[NSDictionary class]] && gKeysNext != NSUIntegerMax - 1) {
    gKeysNext = NSUIntegerMax - 1;  // once per window
    NSMutableDictionary *c = [rule[@"click"] mutableCopy];
    c[@"_window"] = w;
    NSString *err = Click(c);
    Log([NSString stringWithFormat:@"answered \"%@\": click (%@, %@)%@", name, c[@"x"], c[@"y"],
                                   err ? [@" failed: " stringByAppendingString:err] : @""]);
    return err == nil;
  }
  // "close": true, once any keys are spent: end the window's modal session and close it, for notices with no
  // control the driver can reach (Studio Pro's "Missing Devices" exposes only its title-bar widgets).
  if ([rule[@"close"] boolValue] && gKeysNext < NSUIntegerMax) {
    gKeysNext = NSUIntegerMax;  // once per window
    if (w == NSApp.modalWindow) [NSApp abortModal];
    if (w.isSheet && w.sheetParent) [w.sheetParent endSheet:w returnCode:NSModalResponseCancel];
    [w performClose:nil];
    if (w.isVisible) [w orderOut:nil];
    Log([NSString stringWithFormat:@"answered \"%@\": ended its modal session and closed it", name]);
    return YES;
  }
  return NO;
}

static NSString *RenderWindow(NSDictionary *rw);

static void AnswerAlerts(void) {
  NSArray *rules = gRequest[@"answer"];
  if (!NSApp || ![rules isKindOfClass:[NSArray class]] || !rules.count) return;
  NSMutableArray *prompts = [NSMutableArray array];
  if (NSApp.modalWindow) [prompts addObject:NSApp.modalWindow];
  for (NSWindow *w in NSApp.windows)
    if (w.attachedSheet && ![prompts containsObject:w.attachedSheet]) [prompts addObject:w.attachedSheet];
  for (NSWindow *w in prompts)
    for (NSDictionary *rule in rules)
      if ([rule isKindOfClass:[NSDictionary class]] && AnswerWindow(w, rule)) return;
  // A prompt no rule answers (e.g. Cubase's launch-time "Safe Mode", which holds the app before
  // didFinishLaunching so no recipe step ever runs): dump it once after 15 s so the next run can add a rule.
  static NSMutableSet *dumped;
  static NSWindow *seen;
  static NSDate *since;
  NSWindow *top = prompts.firstObject;
  if (top != seen) {
    seen = top;
    since = [NSDate date];
  }
  if (!top) return;
  // Fail fast: a prompt no rule answers that is still up after "fail_unanswered" seconds (request key, default 60;
  // 0 turns it off) ends the job with the prompt's text, instead of waiting out the step's timeout (PBEND: 304 s).
  static NSString *lastTexts;
  static NSWindow *failedFor;
  double failAfter = gRequest[@"fail_unanswered"] ? [gRequest[@"fail_unanswered"] doubleValue] : 60;
  // Cubase's "Perform Audio Export" window (a progress bar with an Abort button) stays up for as long as the render
  // takes; it is not a question, so a long export under load must not be failed as an unanswered prompt.
  BOOL progressWindow = [top.title containsString:@"Perform Audio Export"] || [lastTexts containsString:@"Perform Audio Export"]
    || [top.title hasPrefix:@"Rendering to"];  // FL's command-line render progress window
  if (failAfter > 0 && gBusy && !progressWindow && top != failedFor && [since timeIntervalSinceNow] < -failAfter) {
    failedFor = top;
    Finish(@"error", [NSString stringWithFormat:@"unanswered prompt \"%@\" up %.0f s: %@", top.title ?: @"",
                                                failAfter, lastTexts ?: @"(no text)"], gStepIndex);
  }
  if ([since timeIntervalSinceNow] > -15) return;
  if (!dumped) dumped = [NSMutableSet set];
  NSString *key = [NSString stringWithFormat:@"%p", top];
  if ([dumped containsObject:key]) return;
  [dumped addObject:key];
  NSString *safe = [[top.title ?: @"untitled" componentsSeparatedByCharactersInSet:
      [[NSCharacterSet alphanumericCharacterSet] invertedSet]] componentsJoinedByString:@"-"];
  Log([NSString stringWithFormat:@"unanswered prompt \"%@\" (%@) up 15 s; dumping", top.title,
                                 NSStringFromClass([top class])]);
  Dump([@"unanswered-" stringByAppendingString:safe]);
  // Who owns it: each view class with the image it comes from (an app window vs a plug-in's own box, e.g. a
  // licence nag inside FL), and any text its controls hold.
  NSMutableSet *classesSeen = [NSMutableSet set];
  NSMutableArray *texts = [NSMutableArray array];
  NSMutableArray *stack = [NSMutableArray arrayWithObject:top.contentView.superview ?: top.contentView];
  while (stack.count) {
    NSView *v = stack.lastObject;
    [stack removeLastObject];
    if (!v) continue;
    NSString *cls = NSStringFromClass([v class]);
    if (![classesSeen containsObject:cls]) {
      [classesSeen addObject:cls];
      NSString *image = [[NSBundle bundleForClass:[v class]] bundlePath] ?: @"?";
      Log([NSString stringWithFormat:@"unanswered prompt view %@ from %@", cls, image]);
    }
    if ([v isKindOfClass:[NSTextField class]] && ((NSTextField *)v).stringValue.length)
      [texts addObject:((NSTextField *)v).stringValue];
    if ([v isKindOfClass:[NSButton class]] && ((NSButton *)v).title.length) [texts addObject:((NSButton *)v).title];
    if ([v isKindOfClass:[NSTextView class]] && ((NSTextView *)v).string.length) [texts addObject:((NSTextView *)v).string];
    [stack addObjectsFromArray:v.subviews];
  }
  lastTexts = [texts componentsJoinedByString:@" | "];
  Log([NSString stringWithFormat:@"unanswered prompt size %@, delegate %@, text: %@", NSStringFromSize(top.frame.size),
                                 NSStringFromClass([(id)top.delegate class]) ?: @"none",
                                 [texts componentsJoinedByString:@" | "]]);
  // Self-drawn prompts (FL's CustomWindow) put no text in accessibility: draw it off-screen too, so the
  // next run can tell what it says.
  {
    NSString *match = top.title.length ? [@"^" stringByAppendingString:
                                             [[NSRegularExpression escapedPatternForString:top.title] stringByAppendingString:@"$"]]
                                       : @"<untitled>";
    NSString *why = RenderWindow(@{@"match" : match, @"name" : [@"unanswered-" stringByAppendingString:safe]});
    if (why) Log([NSString stringWithFormat:@"unanswered prompt render: %@", why]);
  }
}

static dispatch_source_t gAnswerTimer;

static void StartAnswering(void) {
  // A main-queue timer keeps firing inside modal run loops, so prompts shown with runModal are answered too.
  gAnswerTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
  dispatch_source_set_timer(gAnswerTimer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), NSEC_PER_SEC / 2,
                            NSEC_PER_SEC / 10);
  dispatch_source_set_event_handler(gAnswerTimer, ^{
    @try {  // never let a throw in our timer take the host app down
      if (!gReleased) AnswerAlerts();  // a released copy's dialogs are the person's to answer
    } @catch (NSException *e) {
      Log([NSString stringWithFormat:@"answer rules threw %@: %@", e.name, e.reason]);
    }
  });
  dispatch_resume(gAnswerTimer);
}


#pragma mark - Off-screen render and in-window clicks


// Metal readback for render_window: CAMetalLayer contents are a CAImageQueue that cannot be read, so the
// driver keeps the last drawable each Metal layer handed out (framebufferOnly turned off first, so its texture
// is readable) and reads that texture back after a forced redraw.
static NSMapTable *gLastDrawable;  // CAMetalLayer -> id<CAMetalDrawable>
static IMP gOrigNextDrawable;
static id RysNextDrawable(id self, SEL _cmd) {
  id d = ((id (*)(id, SEL))gOrigNextDrawable)(self, _cmd);
  if (d) [gLastDrawable setObject:d forKey:self];
  return d;
}
static void HookNextDrawable(void) {
  if (gOrigNextDrawable) return;
  gLastDrawable = [NSMapTable weakToStrongObjectsMapTable];
  Method m = class_getInstanceMethod([CAMetalLayer class], @selector(nextDrawable));
  gOrigNextDrawable = method_setImplementation(m, (IMP)RysNextDrawable);
}
static NSBitmapImageRep *ReadTexture(id<MTLTexture> t) {
  if (!t || t.framebufferOnly) return nil;
  NSUInteger w = t.width, h = t.height, row = w * 4;
  NSMutableData *buf = [NSMutableData dataWithLength:row * h];
  [t getBytes:buf.mutableBytes bytesPerRow:row fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
  BOOL bgra = t.pixelFormat == MTLPixelFormatBGRA8Unorm || t.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB;
  if (bgra) {
    uint8_t *p = buf.mutableBytes;
    for (NSUInteger i = 0; i < w * h; i++) { uint8_t b = p[4 * i]; p[4 * i] = p[4 * i + 2]; p[4 * i + 2] = b; p[4 * i + 3] = 255; }
  } else if (t.pixelFormat != MTLPixelFormatRGBA8Unorm && t.pixelFormat != MTLPixelFormatRGBA8Unorm_sRGB) {
    Log([NSString stringWithFormat:@"render: unsupported Metal pixel format %lu", (unsigned long)t.pixelFormat]);
    return nil;
  }
  unsigned char *planes[1] = {buf.mutableBytes};
  NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:w pixelsHigh:h
      bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
        bytesPerRow:row bitsPerPixel:32];
  memcpy(rep.bitmapData, planes[0], row * h);
  return rep;
}
// While a render_window readback runs, its window reports itself visible: Steinberg's Metal UI skips drawing
// for an occluded window, and every window of a hidden app is occluded. Only that one window, only then.
static __weak NSWindow *gRenderWindow;
static IMP gOrigOcclusion;
static NSWindowOcclusionState RysOcclusion(id self, SEL _cmd) {
  NSWindowOcclusionState st = ((NSWindowOcclusionState (*)(id, SEL))gOrigOcclusion)(self, _cmd);
  return self == gRenderWindow ? (st | NSWindowOcclusionStateVisible) : st;
}
static void HookOcclusion(void) {
  if (gOrigOcclusion) return;
  Method m = class_getInstanceMethod([NSWindow class], @selector(occlusionState));
  gOrigOcclusion = method_setImplementation(m, (IMP)RysOcclusion);
}
// The pop-up menu currently tracking (a click on a custom UI's dropdown opens a native NSMenu whose items
// expose no accessibility labels): recorded from NSMenuDidBeginTrackingNotification for menu_pick.
static __weak NSMenu *gTrackingMenu;
static void WatchMenuTracking(void) {
  static BOOL on;
  if (on) return;
  on = YES;
  [[NSNotificationCenter defaultCenter] addObserverForName:NSMenuDidBeginTrackingNotification object:nil queue:nil
                                                usingBlock:^(NSNotification *n) { gTrackingMenu = n.object; }];
  [[NSNotificationCenter defaultCenter] addObserverForName:NSMenuDidEndTrackingNotification object:nil queue:nil
                                                usingBlock:^(NSNotification *n) {
                                                  if (gTrackingMenu == n.object) gTrackingMenu = nil;
                                                }];
}

// menu_pick RE: choose the item whose title matches in the pop-up menu that is tracking now, then end tracking.
static NSString *MenuPick(NSString *re, NSMutableDictionary *state) {
  NSMutableData *progress = state[@"menu_pick_progress"];
  if (!progress) {
    progress = [NSMutableData dataWithLength:sizeof(RysMenuNavigationState)];
    state[@"menu_pick_progress"] = progress;
  }
  RysMenuNavigationState *nav = progress.mutableBytes;
  NSMenu *m = gTrackingMenu;
  if (!m) {
    if (RysMenuNavigationNext(nav, false, NULL, 0, -1, -1) == RysMenuDone) return nil;
    return @"no pop-up menu is tracking before target selection";
  }
  if (state[@"menu_pick_menu"] && state[@"menu_pick_menu"] != m)
    return @"tracking menu changed before target selection completed";
  state[@"menu_pick_menu"] = m;
  NSMutableData *navigation = [NSMutableData dataWithLength:(NSUInteger)m.numberOfItems];
  unsigned char *items = navigation.mutableBytes;
  for (NSInteger i = 0; i < m.numberOfItems; i++) {
    NSMenuItem *it = [m itemAtIndex:i];
    items[i] = (it.isEnabled ? RysMenuEnabled : 0) |
               (it.isSeparatorItem ? RysMenuSeparator : 0) | (it.isHidden ? RysMenuHidden : 0);
  }
  NSMutableArray *titles = [NSMutableArray array];
  for (NSInteger i = 0; i < m.numberOfItems; i++) {
    NSMenuItem *it = [m itemAtIndex:i];
    [titles addObject:it.title ?: @""];
    if (RysMenuSelectable(items[i]) && Matches(it.title, re)) {
      // Pick it the way a person would: arrow keys from the highlighted item, then Return, posted into the
      // menu's own tracking loop. (performActionForItemAtIndex: fires the action, but a caller that reads the
      // pop-up's result when tracking ends - Cubase's dropdowns - still sees the old selection.)
      NSInteger cur = m.highlightedItem ? [m indexOfItem:m.highlightedItem] : -1;
      RysMenuAction action = RysMenuNavigationNext(nav, true, items, m.numberOfItems, cur, i);
      if (action == RysMenuInvalid)
        return @"pop-up menu has an invalid highlighted item";
      if (action == RysMenuWait)
        return nav->return_sent ? @"waiting for target menu selection to finish" : @"waiting for menu highlight to move";
      // One event pair only. RunStep retries and reads the actual highlight
      // before choosing another arrow or Return; a planned key count is not proof.
      BOOL commit = action == RysMenuReturn;
      unichar character = commit ? '\r' : (action == RysMenuDown ? NSDownArrowFunctionKey : NSUpArrowFunctionKey);
      unsigned short code = commit ? 36 : (action == RysMenuDown ? 125 : 126);
      NSString *characters = [NSString stringWithCharacters:&character length:1];
      for (int t = 0; t < 2; t++) {
        NSEvent *e = [NSEvent keyEventWithType:t ? NSEventTypeKeyUp : NSEventTypeKeyDown location:NSZeroPoint
                                 modifierFlags:(commit ? 0 : NSEventModifierFlagFunction | NSEventModifierFlagNumericPad)
                                     timestamp:[[NSProcessInfo processInfo] systemUptime] windowNumber:0 context:nil
                                    characters:characters charactersIgnoringModifiers:characters isARepeat:NO keyCode:code];
        [NSApp postEvent:e atStart:NO];
      }
      Log([NSString stringWithFormat:@"menu_pick \"%@\" (item %ld, observed highlight %ld): %@",
                                     it.title, (long)i, (long)cur,
                                     commit ? @"Return after target readback" : (action == RysMenuDown ? @"one Down" : @"one Up")]);
      return commit ? @"waiting for target menu selection to finish" : @"waiting for menu highlight to move";
    }
  }
  for (NSInteger i = titles.count; i < m.numberOfItems; i++) [titles addObject:[m itemAtIndex:i].title ?: @""];
  return [NSString stringWithFormat:@"no item matching \"%@\" in %@", re, [titles componentsJoinedByString:@" | "]];
}

static void MetalLayers(CALayer *l, NSMutableArray *out, int depth) {
  if (!l || depth > 40) return;
  if ([l isKindOfClass:[CAMetalLayer class]]) [out addObject:l];
  for (CALayer *k in l.sublayers) MetalLayers(k, out, depth + 1);
}

static NSWindow *WindowTitled(NSString *re) {
  // "<untitled>": the newest untitled window that is not a status-bar item, e.g. the pop-up list Studio Pro's
  // CCL UI opens in a window of its own when one of its dropdowns is clicked.
  if ([re isEqualToString:@"<untitled>"]) {
    for (NSWindow *w in NSApp.windows.reverseObjectEnumerator)
      if (!w.title.length && ![NSStringFromClass([w class]) hasPrefix:@"NSStatusBar"] && w.frame.size.width > 1)
        return w;
    return nil;
  }
  for (NSWindow *w in NSApp.windows)
    if (Matches(w.title, re)) return w;
  return nil;
}

static void ViewFrames(NSView *v, NSView *root, NSMutableArray *out, int depth) {
  if (!v || depth > 60) return;
  // Frames in the root (content view) coordinate space, top-left origin like the PNG.
  NSRect r = [v convertRect:v.bounds toView:root];
  if (!root.isFlipped) r.origin.y = NSHeight(root.bounds) - NSMaxY(r);
  [out addObject:@{@"class" : NSStringFromClass([v class]), @"depth" : @(depth), @"frame" : NSStringFromRect(r),
                   @"hidden" : @(v.isHidden), @"label" : Label(v)}];
  for (NSView *k in v.subviews) ViewFrames(k, root, out, depth + 1);
}

// render_window {match, name}: draw the window's content view off-screen into render-NAME.png (no screen
// capture, so no Screen Recording permission and nothing on screen) and write render-NAME.json with every
// subview's class and frame in the PNG's top-left point space, for custom UIs that expose nothing to
// accessibility (Cubase's VSTGUI Export Audio Mixdown dialog).
static NSString *RenderWindow(NSDictionary *rw) {
  NSWindow *w = WindowTitled(rw[@"match"]);
  if (!w) return [NSString stringWithFormat:@"no window matching \"%@\"", rw[@"match"]];
  NSView *root = w.contentView;
  NSString *name = rw[@"name"] ?: @"window";
  @try {
    NSBitmapImageRep *rep = [root bitmapImageRepForCachingDisplayInRect:root.bounds];
    [root cacheDisplayInRect:root.bounds toBitmapImageRep:rep];
    NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    [png writeToFile:[kDir stringByAppendingPathComponent:[NSString stringWithFormat:@"render-%@.png", name]]
          atomically:YES];
    // Layer-backed custom UIs (VSTGUI draws through CALayer delegates) come out blank from cacheDisplayInRect;
    // also render the layer tree, into render-NAME-layer.png.
    CALayer *layer = (root.superview ?: root).layer ?: root.layer;
    if (layer) {
      NSView *lv = layer == root.layer ? root : root.superview;
      CGFloat scale = w.backingScaleFactor ?: 1;
      size_t pw = (size_t)(NSWidth(lv.bounds) * scale), ph = (size_t)(NSHeight(lv.bounds) * scale);
      CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
      CGContextRef ctx = CGBitmapContextCreate(NULL, pw, ph, 8, 0, cs, (CGBitmapInfo)(CGBitmapInfo)kCGImageAlphaPremultipliedLast);
      CGColorSpaceRelease(cs);
      if (ctx) {
        CGContextScaleCTM(ctx, scale, scale);
        [layer renderInContext:ctx];
        CGImageRef img = CGBitmapContextCreateImage(ctx);
        NSBitmapImageRep *lrep = [[NSBitmapImageRep alloc] initWithCGImage:img];
        [[lrep representationUsingType:NSBitmapImageFileTypePNG properties:@{}]
            writeToFile:[kDir stringByAppendingPathComponent:[NSString stringWithFormat:@"render-%@-layer.png", name]]
             atomically:YES];
        CGImageRelease(img);
        CGContextRelease(ctx);
        Log([NSString stringWithFormat:@"rendered layer tree of %@ (%zux%zu px)", NSStringFromClass([lv class]), pw, ph]);
      }
    }
    NSMutableArray *views = [NSMutableArray array];
    ViewFrames(root, root, views, 0);
    NSMutableArray *layers = [NSMutableArray array];
    __block void (^walk)(CALayer *, int);
    __block __weak void (^weakWalk)(CALayer *, int);
    weakWalk = walk = ^(CALayer *l, int depth) {
      if (!l || depth > 40) return;
      [layers addObject:@{@"class" : NSStringFromClass([l class]), @"depth" : @(depth),
                          @"frame" : NSStringFromRect(l.frame), @"delegate" : NSStringFromClass([l.delegate class]) ?: @"",
                          @"has_contents" : @(l.contents != nil), @"hidden" : @(l.hidden)}];
      for (CALayer *k in l.sublayers) weakWalk(k, depth + 1);
    };
    walk((root.superview ?: root).layer, 0);
    // Metal-drawn UIs (Cubase's SMTG MetalLayerDelegate): read back each Metal layer's last drawable as
    // render-NAME-metal-N.png (prepared by the step's first pass; see RenderWindowStep).
    NSMutableArray *metal = [NSMutableArray array];
    MetalLayers((root.superview ?: root).layer, metal, 0);
    int metalN = 0;
    for (CAMetalLayer *l in metal) {
      id<CAMetalDrawable> d = [gLastDrawable objectForKey:l];
      NSBitmapImageRep *mrep = ReadTexture(d.texture);
      NSString *f = [NSString stringWithFormat:@"render-%@-metal-%d.png", name, metalN];
      if (mrep) [[mrep representationUsingType:NSBitmapImageFileTypePNG properties:@{}]
                    writeToFile:[kDir stringByAppendingPathComponent:f] atomically:YES];
      Log([NSString stringWithFormat:@"render: metal layer %d %@ -> %@", metalN, NSStringFromRect(l.frame),
                                     mrep ? [NSString stringWithFormat:@"%@ (%lux%lu px)", f, (unsigned long)mrep.pixelsWide,
                                                                        (unsigned long)mrep.pixelsHigh]
                                          : (d ? @"texture not readable" : @"no drawable captured")]);
      metalN++;
    }
    NSDictionary *d = @{@"window" : w.title ?: @"", @"class" : NSStringFromClass([w class]),
                        @"content_size" : NSStringFromSize(root.bounds.size),
                        @"pixels" : NSStringFromSize(NSMakeSize(rep.pixelsWide, rep.pixelsHigh)),
                        @"backing_scale" : @(w.backingScaleFactor), @"views" : views, @"layers" : layers};
    [[NSJSONSerialization dataWithJSONObject:d options:NSJSONWritingPrettyPrinted error:nil]
        writeToFile:[kDir stringByAppendingPathComponent:[NSString stringWithFormat:@"render-%@.json", name]]
         atomically:YES];
    Log([NSString stringWithFormat:@"rendered \"%@\" %@ pt -> render-%@.png (%lux%lu px, %lu views)", w.title,
                                   NSStringFromSize(root.bounds.size), name, (unsigned long)rep.pixelsWide,
                                   (unsigned long)rep.pixelsHigh, (unsigned long)views.count]);
  } @catch (NSException *e) {
    return [NSString stringWithFormat:@"render threw %@: %@", e.name, e.reason];
  }
  return nil;
}

// click {match, x, y}: a left mouseDown/mouseUp pair at (x, y), points from the content view's top-left (the
// render_window PNG's space), built in-process with the window's own windowNumber and delivered by
// -[NSWindow sendEvent:]. No CGEvent, no global event, nothing reaches the screen. It runs from a run-loop
// block, not inside the driver's main-queue step, so a modal the click opens (an export progress window)
// cannot stall later steps or the answer rules.
static NSString *Click(NSDictionary *c) {
  NSWindow *w = [c[@"_window"] isKindOfClass:[NSWindow class]] ? c[@"_window"] : WindowTitled(c[@"match"]);
  if (!w) return [NSString stringWithFormat:@"no window matching \"%@\"", c[@"match"]];
  NSView *root = w.contentView;
  CGFloat x = [c[@"x"] doubleValue], y = [c[@"y"] doubleValue];
  NSPoint inRoot = NSMakePoint(x, root.isFlipped ? y : NSHeight(root.bounds) - y);
  NSPoint loc = [root convertPoint:inRoot toView:nil];  // window base coordinates
  NSView *hit = [root.superview ?: root hitTest:[root.superview ?: root convertPoint:loc fromView:nil]];
  Log([NSString stringWithFormat:@"click \"%@\" at (%.0f, %.0f) pt -> window %@, hit %@", w.title, x, y,
                                 NSStringFromPoint(loc), NSStringFromClass([hit class])]);
  NSInteger num = w.windowNumber;
  if ([c[@"post"] boolValue]) {
    // "post": true queues both events through the app's own event loop instead of -[NSWindow sendEvent:].
    // Studio Pro's CCL controls track the mouse after mouseDown by pulling events from the queue, so a
    // mouseUp handed straight to the window never reaches that loop and the click does nothing.
    NSTimeInterval t = [[NSProcessInfo processInfo] systemUptime];
    NSEvent *down = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown location:loc modifierFlags:0
                                       timestamp:t windowNumber:num context:nil eventNumber:0 clickCount:1
                                        pressure:1.0];
    NSEvent *up = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp location:loc modifierFlags:0
                                     timestamp:t + 0.05 windowNumber:num context:nil eventNumber:0 clickCount:1
                                      pressure:0.0];
    [NSApp postEvent:down atStart:NO];
    [NSApp postEvent:up atStart:NO];
    return nil;
  }
  if ([c[@"direct"] boolValue]) {
    // "direct": true hands mouseDown:/mouseUp: straight to the hit view, skipping NSWindow's own routing
    // (which, for a window that is not key in a hidden app, can swallow the first click).
    CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
      @try {
        NSTimeInterval t = [[NSProcessInfo processInfo] systemUptime];
        NSEvent *down = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown location:loc modifierFlags:0
                                           timestamp:t windowNumber:num context:nil eventNumber:0 clickCount:1
                                            pressure:1.0];
        NSEvent *up = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp location:loc modifierFlags:0
                                         timestamp:t + 0.05 windowNumber:num context:nil eventNumber:0 clickCount:1
                                          pressure:0.0];
        NSView *v = [w.contentView.superview ?: w.contentView hitTest:[(w.contentView.superview ?: w.contentView) convertPoint:loc fromView:nil]];
        NSEvent *move = [NSEvent mouseEventWithType:NSEventTypeMouseMoved location:loc modifierFlags:0
                                           timestamp:t - 0.05 windowNumber:num context:nil eventNumber:0 clickCount:0
                                            pressure:0.0];
        [v mouseMoved:move];
        [v mouseDown:down];
        [v mouseUp:up];
      } @catch (NSException *e) {
        Log([NSString stringWithFormat:@"direct click threw %@: %@", e.name, e.reason]);
      }
    });
    CFRunLoopWakeUp(CFRunLoopGetMain());
    return nil;
  }
  CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
    @try {
      NSTimeInterval t = [[NSProcessInfo processInfo] systemUptime];
      NSEvent *down = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown location:loc modifierFlags:0
                                         timestamp:t windowNumber:num context:nil eventNumber:0 clickCount:1
                                          pressure:1.0];
      NSEvent *up = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp location:loc modifierFlags:0
                                       timestamp:t + 0.05 windowNumber:num context:nil eventNumber:0 clickCount:1
                                        pressure:0.0];
      [w sendEvent:down];
      [w sendEvent:up];
    } @catch (NSException *e) {
      Log([NSString stringWithFormat:@"click threw %@: %@", e.name, e.reason]);
    }
  });
  CFRunLoopWakeUp(CFRunLoopGetMain());
  return nil;
}

static void StepsDone(NSString *status, NSString *detail, NSUInteger step);

// A WAV a renderer has finished: RIFF/RF64 with a fmt chunk and a data chunk that holds samples, and the file
// is exactly as long as the chunk sizes say. Studio Pro writes a 734-byte header-only stub for each stem and
// fills it only after the render (a Mono-channel song kept the stubs for over 25 s), so a quiet file is not done.
static BOOL WavComplete(NSString *path) {
  NSFileHandle *h = [NSFileHandle fileHandleForReadingAtPath:path];
  if (!h) return NO;
  unsigned long long size = [h seekToEndOfFile];
  [h seekToFileOffset:0];
  NSData *head = [h readDataOfLength:(NSUInteger)MIN(size, 4096ull)];
  const uint8_t *b = head.bytes;
  if (head.length < 12 || memcmp(b + 8, "WAVE", 4) != 0 || (memcmp(b, "RIFF", 4) != 0 && memcmp(b, "RF64", 4) != 0)) { [h closeFile]; return NO; }
  NSUInteger o = 12; BOOL fmt = NO;
  while (o + 8 <= head.length) {
    uint32_t n; memcpy(&n, b + o + 4, 4);
    if (memcmp(b + o, "fmt ", 4) == 0) fmt = YES;
    if (memcmp(b + o, "data", 4) == 0) {
      [h closeFile];
      if (!fmt) return NO;
      unsigned long long body = o + 8;
      uint32_t riff; memcpy(&riff, b + 4, 4);
      // samples present, and the RIFF length (written last) matches the file; trailing chunks (iXML, LIST) are fine
      return n > 0 && n != 0xFFFFFFFFu && body + n <= size && (memcmp(b, "RF64", 4) == 0 || (unsigned long long)riff + 8 == size);
    }
    o += 8 + n + (n & 1);
  }
  [h closeFile];
  return NO;
}

static void RunStep(NSArray *steps, NSUInteger i, NSDate *stepStart, NSMutableDictionary *state) {
  if (gQuitting) return;  // a warm instance asked to quit: leave the rest to the quit
  if (gReleased) {
    // The person opened their own document mid-job: stop here, and run no close steps, which could close theirs.
    gBusy = NO;
    gClosed = @NO;
    return Finish(@"error", @"the app was opened by the person during the job; it is theirs now", i);
  }
  double total = [gRequest[@"timeout"] doubleValue] ?: 1800;
  if (gWarm && gPhase == 1) total = DBL_MAX;  // close steps carry their own timeouts
  if (-[gStart timeIntervalSinceNow] > total) {
    if (gWarm) return StepsDone(@"timeout", @"whole recipe timed out", i);
    return Finish(@"timeout", @"whole recipe timed out", i);
  }
  if (gAnswerFail) {
    NSString *f = gAnswerFail;
    gAnswerFail = nil;
    if (gWarm) return StepsDone(@"error", f, i);
    return Finish(@"error", f, i);
  }
  if (i >= steps.count) {
    if (gWarm) return StepsDone(@"ok", @"recipe finished", i);
    return Finish(@"ok", @"recipe finished", i);
  }
  gStepIndex = i;
  NSDictionary *s = steps[i];
  double timeout = [s[@"timeout"] doubleValue] ?: 60;
  BOOL optional = [s[@"optional"] boolValue];
  BOOL late = -[stepStart timeIntervalSinceNow] > timeout;
  NSString *why = nil;  // set when the step cannot complete yet

  if (s[@"wait"]) {
    if (-[stepStart timeIntervalSinceNow] < [s[@"wait"] doubleValue]) return Next(steps, i, 0.5, stepStart, state);
  } else if (s[@"wait_window"]) {
    BOOL found = NO;
    for (NSWindow *w in NSApp.windows) found |= Matches(w.title, s[@"wait_window"]);
    if (!found) why = [NSString stringWithFormat:@"no window matching \"%@\"", s[@"wait_window"]];
  } else if (s[@"wait_no_modal"]) {
    BOOL sheet = NO;
    for (NSWindow *w in NSApp.windows) sheet |= w.attachedSheet != nil;
    if (NSApp.modalWindow || sheet) {
      why = [NSString stringWithFormat:@"modal still up: %@", NSApp.modalWindow.title];
      [state removeObjectForKey:@"clear_since"];
    } else if (s[@"quiet"]) {
      // {"wait_no_modal": true, "quiet": N}: no modal or sheet for N seconds in a row (a song still loading raises
      // its prompts one after another), instead of a fixed wait after the first clear moment.
      if (!state[@"clear_since"]) state[@"clear_since"] = [NSDate date];
      if (-[state[@"clear_since"] timeIntervalSinceNow] < [s[@"quiet"] doubleValue]) why = @"waiting for a quiet spell";
    }
  } else if (s[@"menu"] && [s[@"async"] boolValue]) {
    // async: fire from a run-loop block so a modal the item opens (Add Track) can't block
    // the main queue, which carries every later step and the answer rules.
    NSArray *path = s[@"menu"];
    CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
      NSString *e = FireMenu(path);
      Log([NSString stringWithFormat:@"async menu %@ -> %@", path, e ?: @"fired"]);
    });
    CFRunLoopWakeUp(CFRunLoopGetMain());
  } else if (s[@"menu"]) {
    why = FireMenu(s[@"menu"]);
  } else if (s[@"press"]) {
    id hit = nil;
    for (NSWindow *w in SearchOrder(s[@"window"])) if ((hit = FindElement(w, s[@"press"], YES, 0))) break;
    if (!hit) why = [NSString stringWithFormat:@"no button matching \"%@\"", s[@"press"]];
    else if (!Press(hit)) why = [NSString stringWithFormat:@"press refused by %@", NSStringFromClass([hit class])];
  } else if (s[@"set_value"]) {
    NSDictionary *sv = s[@"set_value"];
    id hit = nil;
    for (NSWindow *w in SearchOrder(s[@"window"])) if ((hit = FindElement(w, sv[@"match"], NO, 0))) break;
    why = hit ? SetValue(hit, sv[@"value"]) : [NSString stringWithFormat:@"no field matching \"%@\"", sv[@"match"]];
  } else if (s[@"key"]) {
    why = SendKeyVia(s[@"key"], s[@"mods"], s[@"window"], [s[@"post"] boolValue], [s[@"direct"] boolValue]);
  } else if (s[@"dump"]) {
    Dump(s[@"dump"]);
  } else if (s[@"render_window"]) {
    if (!state[@"prepared"]) {
      // First pass: make the window's Metal layers readable and force a redraw, then render a second later.
      HookNextDrawable();
      HookOcclusion();
      NSWindow *w = WindowTitled(s[@"render_window"][@"match"]);
      gRenderWindow = w;
      [[NSNotificationCenter defaultCenter] postNotificationName:NSWindowDidChangeOcclusionStateNotification object:w];
      NSMutableArray *metal = [NSMutableArray array];
      MetalLayers((w.contentView.superview ?: w.contentView).layer, metal, 0);
      for (CAMetalLayer *l in metal) {
        l.framebufferOnly = NO;
        [l setNeedsDisplay];
      }
      [w.contentView setNeedsDisplay:YES];
      for (NSView *v in w.contentView.subviews) [v setNeedsDisplay:YES];
      [w displayIfNeeded];
      // Steinberg's Metal UI redraws only dirty regions; a 1-pt content resize (restored on the next pass)
      // makes it redraw the whole dialog into a fresh drawable.
      if (w && metal.count) {
        NSSize sz = w.contentView.frame.size;
        state[@"size"] = NSStringFromSize(sz);
        [w setContentSize:NSMakeSize(sz.width + 1, sz.height)];
        id del = [(CALayer *)metal.firstObject delegate];
        if (del && !state[@"logged"]) {
          unsigned int n = 0;
          Method *ms = class_copyMethodList([del class], &n);
          NSMutableArray *names = [NSMutableArray array];
          for (unsigned int k = 0; k < n; k++) [names addObject:NSStringFromSelector(method_getName(ms[k]))];
          free(ms);
          Log([NSString stringWithFormat:@"render: %@ methods %@", NSStringFromClass([del class]),
                                         [names componentsJoinedByString:@" "]]);
          state[@"logged"] = @YES;
        }
      }
      state[@"prepared"] = [NSDate date];
      why = w ? @"redrawing for readback" : [NSString stringWithFormat:@"no window matching \"%@\"", s[@"render_window"][@"match"]];
    } else if (-[state[@"prepared"] timeIntervalSinceNow] < 1.5) {
      why = @"redrawing for readback";
      if (state[@"size"] && -[state[@"prepared"] timeIntervalSinceNow] > 0.5) {
        NSWindow *w = WindowTitled(s[@"render_window"][@"match"]);
        [w setContentSize:NSSizeFromString(state[@"size"])];
        [state removeObjectForKey:@"size"];
        // Ask each Metal layer's delegate to draw now (Steinberg's delegate draws in displayLayer:), so the
        // whole dialog lands in a captured drawable that the next pass reads back.
        NSMutableArray *metal = [NSMutableArray array];
        MetalLayers((w.contentView.superview ?: w.contentView).layer, metal, 0);
        for (CAMetalLayer *l in metal)
          if ([l.delegate respondsToSelector:@selector(displayLayer:)]) [(id<CALayerDelegate>)l.delegate displayLayer:l];
      }
    } else {
      why = RenderWindow(s[@"render_window"]);
      gRenderWindow = nil;
    }
  } else if (s[@"make_key"]) {
    // make_key: make a hidden window main/key so the app's own active-document tracking runs.
    NSWindow *w = WindowTitled(s[@"make_key"]);
    if (!w) why = [NSString stringWithFormat:@"no window matching \"%@\"", s[@"make_key"]];
    else {
      // A hidden app's windows are not visible, and AppKit asserts in -[NSWindow _changeJustMain] when such a
      // window is made main (FL Studio 2026 aborted here, FL-X3 2026-10-21): catch it so the run goes on.
      @try {
        if (w.canBecomeMainWindow) [w makeMainWindow];
      } @catch (NSException *e) {
        Log([NSString stringWithFormat:@"make_key: makeMainWindow threw %@: %@", e.name, e.reason]);
      }
      @try {
        if (w.canBecomeKeyWindow) [w makeKeyWindow];
      } @catch (NSException *e) {
        Log([NSString stringWithFormat:@"make_key: makeKeyWindow threw %@: %@", e.name, e.reason]);
      }
      [[NSNotificationCenter defaultCenter] postNotificationName:NSWindowDidBecomeMainNotification object:w];
      [[NSNotificationCenter defaultCenter] postNotificationName:NSWindowDidBecomeKeyNotification object:w];
      if ([w.delegate respondsToSelector:@selector(windowDidBecomeMain:)])
        [w.delegate windowDidBecomeMain:[NSNotification notificationWithName:NSWindowDidBecomeMainNotification object:w]];
      if ([w.delegate respondsToSelector:@selector(windowDidBecomeKey:)])
        [w.delegate windowDidBecomeKey:[NSNotification notificationWithName:NSWindowDidBecomeKeyNotification object:w]];
      Log([NSString stringWithFormat:@"make_key: key=%@ main=%@ delegate=%@", NSApp.keyWindow.title, NSApp.mainWindow.title, [w.delegate class]]);
    }
  } else if (s[@"click"]) {
    WatchMenuTracking();
    why = Click(s[@"click"]);
  } else if (s[@"menu_pick"]) {
    why = MenuPick(s[@"menu_pick"], state);
  } else if (s[@"wait_file"]) {
    NSString *p = s[@"wait_file"];
    unsigned long long size = FileSize(p);
    NSNumber *last = state[@"size"];
    NSDate *since = state[@"since"];
    if (!size) why = @"file not written yet";
    else if (!last || last.unsignedLongLongValue != size) {
      state[@"size"] = @(size);
      state[@"since"] = [NSDate date];
      why = @"file still growing";
    } else if (-[since timeIntervalSinceNow] < 3) why = @"file still growing";
    if (!s[@"timeout"]) timeout = total;
  } else if (s[@"wait_new_file"]) {
    // For dialogs that write to their own remembered folder instead of an NSSavePanel (Studio Pro's Export
    // Mixdown): the newest file in "dir" matching "glob" that was written after the request started, once its
    // size and modification time have both held still for "settle" seconds (3 by default). Studio Pro sizes the
    // Mixdown at full length up front and then fills it, so size alone called a half-rendered file done.
    // Its path goes into done.json as "exported".
    NSDictionary *wf = s[@"wait_new_file"];
    NSString *dir = [wf[@"dir"] stringByExpandingTildeInPath];
    NSString *glob = wf[@"glob"] ?: @"*.wav";
    NSPredicate *like = [NSPredicate predicateWithFormat:@"SELF LIKE[c] %@", glob];
    NSString *newest = nil;
    NSDate *newestDate = gStart;
    NSDirectoryEnumerator *en = [[NSFileManager defaultManager] enumeratorAtPath:dir];
    for (NSString *rel in en) {
      if (![like evaluateWithObject:rel.lastPathComponent]) continue;
      NSDate *m = en.fileAttributes.fileModificationDate;
      if ([m compare:newestDate] == NSOrderedDescending) {
        newestDate = m;
        newest = [dir stringByAppendingPathComponent:rel];
      }
    }
    unsigned long long size = newest ? FileSize(newest) : 0;
    NSNumber *last = state[@"size"];
    double settle = wf[@"settle"] ? [wf[@"settle"] doubleValue] : 3;
    if (!size) why = [NSString stringWithFormat:@"no new %@ in %@ yet", glob, dir];
    else if (!last || last.unsignedLongLongValue != size || ![state[@"path"] isEqual:newest]
             || ![state[@"mtime"] isEqual:newestDate]) {
      state[@"size"] = @(size);
      state[@"mtime"] = newestDate;
      state[@"path"] = newest;
      state[@"since"] = [NSDate date];
      why = @"file still being written";
    } else if (-[state[@"since"] timeIntervalSinceNow] < settle) why = @"file still being written";
    else [gExported addObject:newest];
    if (!s[@"timeout"]) timeout = total;
  } else if (s[@"find_text"]) {
    // Locate a row by its text in the last render of a window (Vision OCR on render-NAME[-metal-0].png), for
    // self-drawn lists whose layout depends on the project (Cubase's channel list: the "Audio Channels" group row
    // moves with the number of outputs, groups and FX channels). Sets the "found" point {x of the step or the
    // text's centre, y of the text's centre} used by a following ensure_checked {"at_found": true}.
    NSDictionary *ft = s[@"find_text"];
    NSString *name = ft[@"render"] ?: @"ensure";
    NSString *png = [kDir stringByAppendingPathComponent:[NSString stringWithFormat:@"render-%@-metal-0.png", name]];
    if (![[NSFileManager defaultManager] fileExistsAtPath:png])
      png = [kDir stringByAppendingPathComponent:[NSString stringWithFormat:@"render-%@.png", name]];
    NSWindow *w = WindowTitled(ft[@"match"]);
    NSImage *img = [[NSImage alloc] initWithContentsOfFile:png];
    CGImageRef cg = [img CGImageForProposedRect:NULL context:nil hints:nil];
    if (!cg || !w) why = @"no render to read";
    else {
      // Vision's Neural Engine path fails under load or after the ANE service restarts ("e5rtError ... precompiled compute
      // operation", 20261022: every Cubase export failed at 'Output Channels'), and on macOS 27 usesCPUOnly alone does not
      // always keep it off the ANE. Try, in turn: the default, the CPU compute device, the CPU-only flag, the first text
      // revision (an older engine), the fast level, and a half-size image. An attempt counts when it returns no error, has
      // results and one of them matches; every attempt is logged.
      NSError *err = nil;
      BOOL fuzzyHit = NO;
      VNRecognizeTextRequest *req = nil, *best = nil;
      NSArray<NSString *> *attempts = @[ @"default", @"cpu-device", @"cpu-only", @"revision-1", @"fast-x3", @"fast-x2", @"fast-x4", @"fast" ];
      for (NSString *attempt in attempts) {
        VNRecognizeTextRequest *r = [[VNRecognizeTextRequest alloc] init];
        r.recognitionLevel = VNRequestTextRecognitionLevelAccurate;
        r.usesLanguageCorrection = NO;
        r.minimumTextHeight = 0;
        CGImageRef use = cg;
        CGImageRef shrunk = NULL;
        BOOL skip = NO;
        if ([attempt isEqualToString:@"cpu-device"]) {
          typedef NSArray *(*AllDevicesFn)(void);
          AllDevicesFn all = (AllDevicesFn)dlsym(RTLD_DEFAULT, "MLAllComputeDevices");
          id cpuDevice = nil;
          for (id d in (all ? all() : @[])) if ([NSStringFromClass([d class]) containsString:@"CPU"]) cpuDevice = d;
          SEL set = NSSelectorFromString(@"setComputeDevice:forComputeStage:");
          if (cpuDevice && [r respondsToSelector:set]) {
            BOOL (*send)(id, SEL, id, NSString *) = (BOOL (*)(id, SEL, id, NSString *))objc_msgSend;
            send(r, set, cpuDevice, @"VNComputeStageMain");
          } else skip = YES;
        } else if ([attempt isEqualToString:@"cpu-only"]) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
          r.usesCPUOnly = YES;
#pragma clang diagnostic pop
        } else if ([attempt isEqualToString:@"revision-1"]) {
          r.revision = VNRecognizeTextRequestRevision1;
        } else if ([attempt hasPrefix:@"fast"]) {
          r.recognitionLevel = VNRequestTextRecognitionLevelFast;
          double up = [attempt hasSuffix:@"x2"] ? 2 : [attempt hasSuffix:@"x3"] ? 3 : [attempt hasSuffix:@"x4"] ? 4 : 1;
          if (up > 1) {
          size_t hw = CGImageGetWidth(cg) * up, hh = CGImageGetHeight(cg) * up;
          CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
          CGContextRef ctx = CGBitmapContextCreate(NULL, hw, hh, 8, 0, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
          CGColorSpaceRelease(cs);
          if (ctx) {
            CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
            CGContextDrawImage(ctx, CGRectMake(0, 0, hw, hh), cg);
            shrunk = CGBitmapContextCreateImage(ctx);
            CGContextRelease(ctx);
          }
          if (shrunk) use = shrunk; else skip = YES;
          }
        }
        if (skip) { Log([NSString stringWithFormat:@"find_text attempt %@: not available", attempt]); continue; }
        NSError *e = nil;
        BOOL ok = [[[VNImageRequestHandler alloc] initWithCGImage:use options:@{}] performRequests:@[ r ] error:&e];
        if (shrunk) CGImageRelease(shrunk);
        BOOL matched = NO;
        BOOL fuzzy = [attempt hasPrefix:@"fast"];  // only the garbling engine needs the tolerance
        for (VNRecognizedTextObservation *o in r.results) {
          NSString *t = [o topCandidates:1].firstObject.string;
          if (t && Matches(t, ft[@"text"])) { matched = YES; break; }
        }
        if (!matched && fuzzy && BestFuzzy(r.results, ft[@"text"])) matched = YES;
        Log([NSString stringWithFormat:@"find_text attempt %@: %@, %lu results%@", attempt, ok && !e ? @"ok" : @"failed",
                                       (unsigned long)r.results.count, matched ? @", matched" : (e ? [NSString stringWithFormat:@" (%@)", e] : @"")]);
        if (r.results.count && !best) best = r;
        if (!e && matched) { req = r; err = nil; fuzzyHit = fuzzy; break; }
        err = e ?: err;
      }
      if (!req) req = best ?: [[VNRecognizeTextRequest alloc] init];
      double pw = CGImageGetWidth(cg), ph = CGImageGetHeight(cg);
      double scale = pw / MAX(1.0, w.contentView.bounds.size.width);
      NSDictionary *hit = nil;
      NSMutableArray *read = [NSMutableArray array];
      VNRecognizedTextObservation *fuzzyBest = fuzzyHit ? BestFuzzy(req.results, ft[@"text"]) : nil;
      for (VNRecognizedTextObservation *o in req.results) {
        NSString *t = [o topCandidates:1].firstObject.string;
        if (t) [read addObject:t];
        if (!Matches(t, ft[@"text"]) && !(fuzzyBest && o == fuzzyBest)) continue;
        CGRect b = o.boundingBox;  // normalised, origin bottom-left
        double cx = (b.origin.x + b.size.width / 2) * pw / scale, cy = (1 - b.origin.y - b.size.height / 2) * ph / scale;
        hit = @{@"x" : ft[@"x"] ?: @(cx), @"y" : @(cy), @"text" : t};
        break;
      }
      if (!hit) {
        why = [NSString stringWithFormat:@"text /%@/ not found in %@ (%@)", ft[@"text"], png.lastPathComponent, err ?: @"no error"];
        Log([NSString stringWithFormat:@"find_text read %lu strings: %@", (unsigned long)read.count,
                                       [read componentsJoinedByString:@" | "]]);
      }
      else {
        gFound = hit;
        Log([NSString stringWithFormat:@"find_text: \"%@\" at (%.0f, %.0f) pt", hit[@"text"], [hit[@"x"] doubleValue], [hit[@"y"] doubleValue]]);
      }
    }
  } else if (s[@"ensure_checked"]) {
    // A self-drawn checkbox whose state the app remembers between exports (Cubase's channel ticks): read the
    // box at (x, y) points from the last render_window of that window ("render": its name; the Metal readback
    // render-NAME-metal-0.png, else render-NAME.png), call it ticked when the mean brightness of a 5x5-point patch
    // there is above "threshold" (default 0.3: Cubase ticked 0.45, clear 0.15), and click it once only when that differs from "checked".
    NSMutableDictionary *ec = [s[@"ensure_checked"] mutableCopy];
    if ([ec[@"at_found"] boolValue] && gFound) {
      ec[@"x"] = ec[@"x"] ?: gFound[@"x"];
      ec[@"y"] = gFound[@"y"];
    }
    NSString *name = ec[@"render"] ?: @"ensure";
    NSString *png = [kDir stringByAppendingPathComponent:[NSString stringWithFormat:@"render-%@-metal-0.png", name]];
    if (![[NSFileManager defaultManager] fileExistsAtPath:png])
      png = [kDir stringByAppendingPathComponent:[NSString stringWithFormat:@"render-%@.png", name]];
    NSBitmapImageRep *rep = (NSBitmapImageRep *)[NSBitmapImageRep imageRepWithContentsOfFile:png];
    NSWindow *w = WindowTitled(ec[@"match"]);
    if (!rep || !w) why = [NSString stringWithFormat:@"no render %@ or no window", png.lastPathComponent];
    else {
      double scale = rep.pixelsWide / MAX(1.0, w.contentView.bounds.size.width);
      double x = [ec[@"x"] doubleValue] * scale, y = [ec[@"y"] doubleValue] * scale, half = 2.5 * scale, sum = 0;
      int n = 0;
      for (int px = (int)(x - half); px <= (int)(x + half); px++)
        for (int py = (int)(y - half); py <= (int)(y + half); py++) {
          NSColor *c = [[rep colorAtX:px y:py] colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
          if (!c) continue;
          sum += (c.redComponent + c.greenComponent + c.blueComponent) / 3.0;
          n++;
        }
      double mean = n ? sum / n : 0;
      BOOL ticked = mean > (ec[@"threshold"] ? [ec[@"threshold"] doubleValue] : 0.3);
      BOOL want = [ec[@"checked"] boolValue];
      Log([NSString stringWithFormat:@"ensure_checked (%@, %@): brightness %.2f -> %@, want %@", ec[@"x"], ec[@"y"], mean,
                                     ticked ? @"ticked" : @"clear", want ? @"ticked" : @"clear"]);
      if (ticked != want) why = Click(@{@"match" : ec[@"match"], @"x" : ec[@"x"], @"y" : ec[@"y"]});
    }
  } else if (s[@"wait_written"]) {
    // Every "glob" file this app created since this step started (written.txt, see NoteWritten), wherever its
    // dialog put it, once there are at least "min" and none has changed for "settle" s. All go to "exported".
    NSDictionary *wf = [s[@"wait_written"] isKindOfClass:[NSDictionary class]] ? s[@"wait_written"] : @{};
    NSPredicate *like = [NSPredicate predicateWithFormat:@"SELF LIKE[c] %@", wf[@"glob"] ?: @"*.wav"];
    NSString *logText = [NSString stringWithContentsOfFile:[kDir stringByAppendingPathComponent:@"written.txt"]
                                                  encoding:NSUTF8StringEncoding error:nil] ?: @"";
    NSMutableDictionary *seen = [NSMutableDictionary dictionary];
    for (NSString *line in [logText componentsSeparatedByString:@"\n"]) {
      if (!line.length || ![like evaluateWithObject:line.lastPathComponent]) continue;
      NSDictionary *a = [[NSFileManager defaultManager] attributesOfItemAtPath:line error:nil];
      if (!a || [a.fileModificationDate compare:gStart] != NSOrderedDescending) continue;
      if ([line containsString:@"/Cache/"]) continue;  // render caches (Studio Pro's Cache/Audio)
      seen[line] = [NSString stringWithFormat:@"%llu@%f", a.fileSize, a.fileModificationDate.timeIntervalSinceReferenceDate];
    }
    NSUInteger min = wf[@"min"] ? [wf[@"min"] unsignedIntegerValue] : 1;
    double settle = wf[@"settle"] ? [wf[@"settle"] doubleValue] : 10;
    if (seen.count < min) why = [NSString stringWithFormat:@"%lu of at least %lu written files", (unsigned long)seen.count, (unsigned long)min];
    else if (![state[@"seen"] isEqual:seen]) {
      state[@"seen"] = seen;
      state[@"since"] = [NSDate date];
      why = @"files still being written";
    } else if (-[state[@"since"] timeIntervalSinceNow] < settle) why = @"files still being written";
    else for (NSString *path in [seen.allKeys sortedArrayUsingSelector:@selector(compare:)]) [gExported addObject:path];
    if (!s[@"timeout"]) timeout = total;
  } else if (s[@"wait_new_files"]) {
    // A dialog that writes MANY files into its own folder in one pass (Studio Pro's Export Stems): every
    // "glob" file under "dir" modified since the request started (an 11 s song's stems were done before this
    // step began), once there are at least "min" (default 1) and
    // none of them has changed size or modification time for "settle" seconds (default 10). All of them go
    // into done.json's "exported" list.
    NSDictionary *wf = s[@"wait_new_files"];
    NSString *dir = [wf[@"dir"] stringByExpandingTildeInPath];
    NSPredicate *like = [NSPredicate predicateWithFormat:@"SELF LIKE[c] %@", wf[@"glob"] ?: @"*.wav"];
    NSMutableDictionary *seen = [NSMutableDictionary dictionary];
    NSDirectoryEnumerator *en = [[NSFileManager defaultManager] enumeratorAtPath:dir];
    for (NSString *rel in en) {
      if (![like evaluateWithObject:rel.lastPathComponent]) continue;
      NSDate *m = en.fileAttributes.fileModificationDate;
      if ([m compare:gStart] != NSOrderedDescending) continue;  // since the request: a fast export can finish before this step starts
      seen[[dir stringByAppendingPathComponent:rel]] =
          [NSString stringWithFormat:@"%llu@%f", en.fileAttributes.fileSize, m.timeIntervalSinceReferenceDate];
    }
    NSUInteger min = wf[@"min"] ? [wf[@"min"] unsignedIntegerValue] : 1;
    double settle = wf[@"settle"] ? [wf[@"settle"] doubleValue] : 10;
    // "ready": "wav" polls readiness instead of waiting out a fixed quiet time: every file must be a finished WAV
    // (data chunk with samples, length matching the file) and unchanged for "settle" (then a short 2 s default).
    BOOL wavReady = [wf[@"ready"] isEqual:@"wav"];
    if (wavReady && wf[@"settle"] == nil) settle = 2;
    NSUInteger incomplete = 0;
    if (wavReady) for (NSString *path in seen) if (!WavComplete(path)) incomplete++;
    if (seen.count < min) why = [NSString stringWithFormat:@"%lu of at least %lu new files in %@", (unsigned long)seen.count, (unsigned long)min, dir];
    else if (wavReady && incomplete) {
      state[@"seen"] = seen;
      state[@"since"] = [NSDate date];
      why = [NSString stringWithFormat:@"%lu of %lu stems not finished (header only or short)", (unsigned long)incomplete, (unsigned long)seen.count];
    } else if (![state[@"seen"] isEqual:seen]) {
      state[@"seen"] = seen;
      state[@"since"] = [NSDate date];
      why = @"files still being written";
    } else if (-[state[@"since"] timeIntervalSinceNow] < settle) why = @"files still being written";
    else for (NSString *path in [seen.allKeys sortedArrayUsingSelector:@selector(compare:)]) [gExported addObject:path];
    if (!s[@"timeout"]) timeout = total;
  } else if (s[@"open_document"]) {
    NSDictionary *od = [s[@"open_document"] isKindOfClass:[NSDictionary class]] ? s[@"open_document"]
                                                                               : @{@"path" : s[@"open_document"]};
    NSString *re = od[@"window"];
    BOOL modal = NSApp.modalWindow != nil;
    for (NSWindow *w in NSApp.windows) modal |= w.attachedSheet != nil;
    if ([od[@"open_now"] boolValue] && !state[@"route"]) {
      // A warm instance: no launch opened this copy, so open it in-process right away.
      state[@"route"] = OpenDocument(od[@"path"]);
      Log([NSString stringWithFormat:@"opened %@ via %@ (warm)", od[@"path"], state[@"route"]]);
    }
    if (od[@"ready_idle"]) {
      // Apps whose windows have no titles (Live): trust the launch's own open and call the project loaded
      // once the grace has passed and no modal or sheet has been up for ready_idle seconds in a row.
      if (modal || !state[@"idle_since"]) state[@"idle_since"] = [NSDate date];
      double idle = -[state[@"idle_since"] timeIntervalSinceNow];
      if (-[stepStart timeIntervalSinceNow] < [(od[@"grace"] ?: @10) doubleValue] ||
          idle < [od[@"ready_idle"] doubleValue])
        why = modal ? [NSString stringWithFormat:@"modal up: %@ \"%@\"", NSStringFromClass([NSApp.modalWindow class]),
                                                 NSApp.modalWindow.title]
                    : @"waiting for the app to settle";
      else
        Log([NSString stringWithFormat:@"project taken as loaded after %.0f s with no modal", idle]);
    } else if (re && WindowMatching(re)) {
      Log([NSString stringWithFormat:@"project window up (%@)", state[@"route"] ?: @"opened at launch"]);
    } else if (!state[@"route"]) {
      // Give the launch's own odoc a moment first, so the project is not opened twice.
      if (-[stepStart timeIntervalSinceNow] < [(od[@"grace"] ?: @10) doubleValue]) {
        why = @"waiting for the launch to open the project";
      } else {
        state[@"route"] = OpenDocument(od[@"path"]);
        Log([NSString stringWithFormat:@"opened %@ via %@", od[@"path"], state[@"route"]]);
        if (re) why = @"waiting for the project window";
      }
    } else if (re) {
      why = [NSString stringWithFormat:@"no window matching \"%@\" after %@", re, state[@"route"]];
    }
  } else if (s[@"close_document"]) {
    // close_document {"menu": [...], "window": RE, "settle": N}: close the open project without saving it,
    // keeping the app running (warm mode). First pass: cancel any dialog the recipe left up, mark NSDocuments
    // clean and close them, fire the app's own close item from a run-loop block (its "save changes?" prompt
    // runs a modal loop). Later passes answer that prompt with Don't Save / No and pass once no window
    // matches RE and no modal is up (with "settle" s of quiet after the close fired, default 2).
    NSDictionary *cd = [s[@"close_document"] isKindOfClass:[NSDictionary class]] ? s[@"close_document"] : @{};
    if (!state[@"fired"]) {
      gSaveURL = nil;
      gOpenURL = nil;
      BOOL cancelled = CancelModals(@"before close");
      for (NSDocument *d in [[NSDocumentController sharedDocumentController] documents]) {
        [d updateChangeCount:NSChangeCleared];  // a test copy: never save it
        [d close];
      }
      NSArray *path = cd[@"menu"];
      if ([cd[@"window_close"] boolValue] && [cd[@"window"] length]) {
        // Close the project's own window the way its close button does (Cubase greys File > Close in a hidden
        // instance: no window can become key there, so it has no active project). From a run-loop block, since
        // the app's "save changes?" prompt runs a modal loop.
        NSString *re = cd[@"window"];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)((cancelled ? 1.5 : 0.2) * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
                         CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
                           NSWindow *w = WindowTitled(re);
                           @try {
                             if (!w) {
                               Log(@"close window: none left");
                             } else if (w.styleMask & NSWindowStyleMaskClosable) {
                               Log([NSString stringWithFormat:@"close window: performClose: on \"%@\"", w.title]);
                               [w performClose:nil];
                             } else if (![w.delegate respondsToSelector:@selector(windowShouldClose:)] ||
                                        [w.delegate windowShouldClose:w]) {
                               Log([NSString stringWithFormat:@"close window: close on \"%@\" (not closable)", w.title]);
                               [w close];
                             } else {
                               Log([NSString stringWithFormat:@"close window: \"%@\" refused to close", w.title]);
                             }
                           } @catch (NSException *e) {
                             Log([NSString stringWithFormat:@"close window threw %@: %@", e.name, e.reason]);
                           }
                         });
                         CFRunLoopWakeUp(CFRunLoopGetMain());
                       });
      } else if ([path isKindOfClass:[NSArray class]] && path.count) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)((cancelled ? 1.5 : 0.2) * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
                         CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
                           NSString *e = FireMenu(path);
                           Log([NSString stringWithFormat:@"close menu %@ -> %@", path, e ?: @"fired"]);
                         });
                         CFRunLoopWakeUp(CFRunLoopGetMain());
                       });
      }
      state[@"fired"] = [NSDate date];
      why = @"closing the project";
    } else {
      PressDontSave(@"closing");
      BOOL modal = NSApp.modalWindow != nil;
      for (NSWindow *w in NSApp.windows) modal |= w.attachedSheet != nil;
      double settle = cd[@"settle"] ? [cd[@"settle"] doubleValue] : 2;
      NSString *re = cd[@"window"];
      if (re.length && WindowMatching(re)) why = [NSString stringWithFormat:@"window matching \"%@\" still open", re];
      else if (modal) why = [NSString stringWithFormat:@"modal up: %@", NSApp.modalWindow.title];
      else if (-[state[@"fired"] timeIntervalSinceNow] < settle) why = @"settling after the close";
    }
  } else if (s[@"quit"]) {
    gSaveURL = nil;
    gOpenURL = nil;
    for (NSDocument *d in [[NSDocumentController sharedDocumentController] documents]) {
      [d updateChangeCount:NSChangeCleared];  // a test copy: never save it
      [d close];
    }
    Finish(@"ok", @"recipe finished; quitting", i);
    BeginQuit(@"recipe");
    return Advance(steps, i, state);  // later steps may answer a "save changes?" prompt
  } else {
    why = [NSString stringWithFormat:@"unknown step %@", s];
    late = YES;
  }

  if (!why) {
    Log([NSString stringWithFormat:@"step %lu ok: %@", (unsigned long)i, s]);
    return Advance(steps, i, state);
  }
  if (!late) return Next(steps, i, 0.5, stepStart, state);
  if (optional) {
    Log([NSString stringWithFormat:@"step %lu skipped (%@): %@", (unsigned long)i, why, s]);
    return Advance(steps, i, state);
  }
  Dump([NSString stringWithFormat:@"failed-step-%lu", (unsigned long)i]);
  if (gWarm) return StepsDone(@"error", why, i);  // close the project, then idle (or quit if that fails)
  Finish(@"error", why, i);
  // Don't leave a hidden copy of the app running after a failed recipe; quit it the same normal way.
  if (gRequest[@"quit_on_error"] && ![gRequest[@"quit_on_error"] boolValue]) return;
  gSaveURL = nil;
  gOpenURL = nil;
  BeginQuit(@"failed step");
}

#pragma mark - Warm mode

static void WriteWarmState(NSString *stateName) {
  if (!gWarm || gReleased) return;
  NSDictionary *d = @{
    @"pid" : @(getpid()),
    @"bundle_id" : [[NSBundle mainBundle] bundleIdentifier] ?: @"",
    @"executable" : [[NSBundle mainBundle] executablePath] ?: @"",
    @"dylib" : gDylib ?: @"",
    @"state" : stateName,
    @"request_id" : gRequestId ?: @"",
    @"launched_epoch" : @(gLaunchedEpoch),
    @"updated_epoch" : @([[NSDate date] timeIntervalSince1970]),
    @"idle_quit" : @(gIdleQuit),
  };
  NSData *j = [NSJSONSerialization dataWithJSONObject:d options:NSJSONWritingPrettyPrinted error:nil];
  [j writeToFile:[kDir stringByAppendingPathComponent:@"warm.json"] atomically:YES];
}

static void WarmQuit(NSString *why) {
  if (gQuitting || gReleased) return;  // a released copy is the person's: they quit it
  gQuitting = YES;
  WriteWarmState(@"quitting");
  Log([NSString stringWithFormat:@"warm instance quitting: %@", why]);
  gSaveURL = nil;
  gOpenURL = nil;
  BeginQuit(why);
}

static void GoIdle(void) {
  gBusy = NO;
  gIdleSince = [NSDate date];
  WriteWarmState(@"idle");
  Log([NSString stringWithFormat:@"warm: idle after request %@", gRequestId]);
}

// The close has run (or failed): report the request, then wait for the next one, or quit when the project
// could not be closed (the next job then gets a fresh instance).
static void CloseDone(BOOL closed, NSString *why) {
  // A close step that found no project window, when the app has no document and no window of its own left
  // (the project never opened: Cubase refused it or it timed out), has nothing to close: go idle. Quitting
  // here (10-22) left Cubase stuck "quitting" at full CPU with every queued job behind it.
  if (!closed && [why hasPrefix:@"no window matching"] && ![[NSDocumentController sharedDocumentController] documents].count) {
    BOOL open = NO;
    for (NSWindow *w in NSApp.windows) open |= w.isVisible && w.title.length && w != NSApp.modalWindow;
    if (!open) {
      Log([NSString stringWithFormat:@"warm: close found nothing open (%@): staying warm", why]);
      closed = YES;
    }
  }
  gClosed = @(closed);
  if (!closed) Log([NSString stringWithFormat:@"warm: close failed (%@)", why]);
  Finish(gResult[@"status"], gResult[@"detail"], [gResult[@"step"] unsignedIntegerValue]);
  if (closed) GoIdle();
  else WarmQuit(@"close failed");
}

static void StepsDone(NSString *status, NSString *detail, NSUInteger step) {
  if (gPhase == 1) return CloseDone([status isEqualToString:@"ok"], detail);
  gResult = @{@"status" : status, @"detail" : detail ?: @"", @"step" : @(step)};
  Log([NSString stringWithFormat:@"warm: steps %@ at step %lu (%@); closing the project", status,
                                 (unsigned long)step, detail]);
  gPhase = 1;
  NSArray *close = [gRequest[@"close"] isKindOfClass:[NSArray class]] ? gRequest[@"close"] : @[];
  if (!close.count) return CloseDone(NO, @"the request names no close steps");
  Next(close, 0, 0.5, [NSDate date], [NSMutableDictionary dictionary]);
}

static void StartRequest(NSDictionary *r) {
  gRequest = r;
  gStart = [NSDate date];
  gPanels = [NSMutableArray array];
  gExported = [NSMutableArray array];
  gSaveURL = [r[@"save_url"] isKindOfClass:[NSString class]] ? [NSURL fileURLWithPath:r[@"save_url"]] : nil;
  gDirURL = [r[@"dir_url"] isKindOfClass:[NSString class]] ? [NSURL fileURLWithPath:r[@"dir_url"] isDirectory:YES] : nil;
  gOpenURL = [r[@"open_url"] isKindOfClass:[NSString class]] ? [NSURL fileURLWithPath:r[@"open_url"]] : nil;
  gRequestId = [r[@"id"] isKindOfClass:[NSString class]] ? r[@"id"] : @"";
  if (r[@"idle_quit"]) gIdleQuit = [r[@"idle_quit"] doubleValue];
  gPhase = 0;
  gClosed = nil;
  gResult = nil;
  gStepIndex = 0;
  gKeysWindow = nil;
  gBusy = YES;
  WriteWarmState(@"busy");
}

// Once a second in a warm instance: a {"quit": true} request is honoured at any time; a request with a new
// id only while idle (the launcher holds a per-app queue lock, so it never sends one while a job runs).
static void WarmPoll(void) {
  if (gQuitting) return;
  if (gReleased) {
    // A released copy takes no jobs. Only a request that says so explicitly ({"quit": true, "release_quit":
    // true}, written by hand, never by the launcher) ends it: the copy that launched a test can clean up.
    NSString *req = [kDir stringByAppendingPathComponent:@"request.json"];
    NSData *data = [NSData dataWithContentsOfFile:req];
    id r = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    if ([r isKindOfClass:[NSDictionary class]] && [r[@"quit"] boolValue] && [r[@"release_quit"] boolValue]) {
      [[NSFileManager defaultManager] removeItemAtPath:req error:nil];
      gReleased = NO;
      gStayHidden = NO;
      WarmQuit(@"explicit release_quit");
    }
    return;
  }
  NSString *req = [kDir stringByAppendingPathComponent:@"request.json"];
  NSData *data = [NSData dataWithContentsOfFile:req];
  id r = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
  NSString *bundle = [[NSBundle mainBundle] bundleIdentifier];
  if ([r isKindOfClass:[NSDictionary class]] && [r[@"bundle_id"] isEqual:bundle]) {
    NSString *rid = [r[@"id"] isKindOfClass:[NSString class]] ? r[@"id"] : @"";
    NSString *work = [kDir stringByAppendingPathComponent:@"request.working.json"];
    if ([r[@"quit"] boolValue]) {
      [[NSFileManager defaultManager] removeItemAtPath:work error:nil];
      [[NSFileManager defaultManager] moveItemAtPath:req toPath:work error:nil];
      if (gBusy) {
        gResult = @{@"status" : @"error", @"detail" : @"quit requested while a job was running", @"step" : @(gStepIndex)};
        gClosed = @NO;
        Finish(gResult[@"status"], gResult[@"detail"], gStepIndex);
      }
      return WarmQuit(gBusy ? @"quit requested during a job" : @"quit requested");
    }
    if (!gBusy && rid.length && ![rid isEqualToString:gRequestId ?: @""]) {
      [[NSFileManager defaultManager] removeItemAtPath:work error:nil];
      if (![[NSFileManager defaultManager] moveItemAtPath:req toPath:work error:nil]) return;
      StartRequest(r);
      Log([NSString stringWithFormat:@"warm: request %@ taken by pid %d", rid, getpid()]);
      NSArray *steps = [r[@"steps"] isKindOfClass:[NSArray class]] ? r[@"steps"] : @[];
      Next(steps, 0, 0.2, [NSDate date], [NSMutableDictionary dictionary]);
      return;
    }
  }
  if (!gBusy && gIdleSince && -[gIdleSince timeIntervalSinceNow] > gIdleQuit)
    WarmQuit([NSString stringWithFormat:@"idle for %.0f s", gIdleQuit]);
}

static dispatch_source_t gWarmTimer;

static void StartWarm(void) {
  gWarmTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
  dispatch_source_set_timer(gWarmTimer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC,
                            NSEC_PER_SEC / 5);
  dispatch_source_set_event_handler(gWarmTimer, ^{
    @try {
      WarmPoll();
    } @catch (NSException *e) {
      Log([NSString stringWithFormat:@"warm poll threw %@: %@", e.name, e.reason]);
    }
  });
  dispatch_resume(gWarmTimer);
  [[NSNotificationCenter defaultCenter] addObserverForName:NSApplicationWillTerminateNotification
                                                    object:nil
                                                     queue:nil
                                                usingBlock:^(NSNotification *n) {
                                                  [[NSFileManager defaultManager]
                                                      removeItemAtPath:[kDir stringByAppendingPathComponent:@"warm.json"]
                                                                 error:nil];
                                                  Log(@"warm instance terminating");
                                                }];
}

__attribute__((constructor)) static void RysUpExportDriverInit(void) {
  const char *inserted = getenv("DYLD_INSERT_LIBRARIES");
  if (inserted) gDylib = [NSString stringWithUTF8String:inserted];
  // Keep the injection out of every helper the app spawns (plug-in scanners, AU hosts).
  unsetenv("DYLD_INSERT_LIBRARIES");
  NSString *bundle = [[NSBundle mainBundle] bundleIdentifier];
  if (!bundle.length) return;
  kDir = [kRoot stringByAppendingPathComponent:bundle];
  // A second (third …) copy of the same app launched beside the first gets its own request folder
  // (<bundle id>.slotN) and so its own request.json, done.json and warm.json: export.py --slot N.
  const char *slot = getenv("RYSUP_EXPORT_SLOT");
  if (slot && atoi(slot) > 0)
    kDir = [kRoot stringByAppendingPathComponent:[NSString stringWithFormat:@"%@.slot%d", bundle, atoi(slot)]];
  NSFileManager *fm = [NSFileManager defaultManager];
  NSString *req = [kDir stringByAppendingPathComponent:@"request.json"];
  NSData *d = [NSData dataWithContentsOfFile:req];
  id r = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil;
  if (![r isKindOfClass:[NSDictionary class]] || ![r[@"bundle_id"] isEqual:bundle]) return;  // stay inert
  NSString *work = [kDir stringByAppendingPathComponent:@"request.working.json"];
  [fm removeItemAtPath:work error:nil];
  if (![fm moveItemAtPath:req toPath:work error:nil]) return;  // another instance took it
  gRequest = r;
  gStart = [NSDate date];
  gPanels = [NSMutableArray array];
  gExported = [NSMutableArray array];
  strlcpy(gWrittenLog, [[kDir stringByAppendingPathComponent:@"written.txt"] fileSystemRepresentation], sizeof gWrittenLog);
  if ([r[@"save_url"] isKindOfClass:[NSString class]]) gSaveURL = [NSURL fileURLWithPath:r[@"save_url"]];
  if ([r[@"dir_url"] isKindOfClass:[NSString class]])
    gDirURL = [NSURL fileURLWithPath:r[@"dir_url"] isDirectory:YES];
  if ([r[@"open_url"] isKindOfClass:[NSString class]]) gOpenURL = [NSURL fileURLWithPath:r[@"open_url"]];
  gStayHidden = r[@"stay_hidden"] ? [r[@"stay_hidden"] boolValue] : YES;
  Log([NSString stringWithFormat:@"export driver loaded in %@ pid %d", bundle, getpid()]);
  HookSavePanels();
  HookActivation();
  HookExportViewer();
  StartAnswering();
  if ([r[@"warm"] boolValue]) {
    gWarm = YES;
    gLaunchedEpoch = [[NSDate date] timeIntervalSince1970];
    StartRequest(r);
    StartWarm();
    Log([NSString stringWithFormat:@"warm mode: request %@, idle limit %.0f s", gRequestId, gIdleQuit]);
  }
  [[NSNotificationCenter defaultCenter] addObserverForName:NSApplicationDidFinishLaunchingNotification
                                                    object:nil
                                                     queue:[NSOperationQueue mainQueue]
                                                usingBlock:^(NSNotification *n) {
                                                  if (gStayHidden) [NSApp hide:nil];
                                                  HookForeignOpens();
                                                  NSArray *steps = gRequest[@"steps"];
                                                  if (![steps isKindOfClass:[NSArray class]]) steps = @[ @{@"dump" : @"launch"} ];
                                                  Next(steps, 0, 2, [NSDate date], [NSMutableDictionary dictionary]);
                                                }];
}
