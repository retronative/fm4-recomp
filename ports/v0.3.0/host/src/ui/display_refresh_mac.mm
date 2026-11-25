// Copyright (c) 2026, FM4 Recomp contributors. BSD 3-Clause License.
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include <CoreGraphics/CoreGraphics.h>
#include <SDL3/SDL.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

@interface FM4ResolutionLoadingView : NSView {
  NSTextField* caption_;
  NSProgressIndicator* spinner_;
}
- (void)setTargetLabel:(NSString*)label;
@end

@implementation FM4ResolutionLoadingView

- (instancetype)initWithFrame:(NSRect)frame {
  self = [super initWithFrame:frame];
  if (!self) return nil;

  self.translatesAutoresizingMaskIntoConstraints = NO;
  self.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
  self.wantsLayer = YES;
  self.layer.backgroundColor = [NSColor colorWithWhite:0.07 alpha:0.96].CGColor;
  self.layer.cornerRadius = 12;
  self.layer.borderWidth = 1;
  self.layer.borderColor = [NSColor colorWithWhite:1 alpha:0.24].CGColor;

  spinner_ = [[NSProgressIndicator alloc] initWithFrame:NSZeroRect];
  spinner_.style = NSProgressIndicatorStyleSpinning;
  spinner_.indeterminate = YES;
  spinner_.translatesAutoresizingMaskIntoConstraints = NO;
  [self addSubview:spinner_];
#if !__has_feature(objc_arc)
  [spinner_ release];  // Retained by the view.
#endif

  caption_ = [NSTextField labelWithString:@""];
  caption_.font = [NSFont systemFontOfSize:16 weight:NSFontWeightSemibold];
  caption_.textColor = NSColor.whiteColor;
  caption_.lineBreakMode = NSLineBreakByTruncatingTail;
  caption_.translatesAutoresizingMaskIntoConstraints = NO;
  [self addSubview:caption_];
  [NSLayoutConstraint activateConstraints:@[
    [spinner_.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:18],
    [spinner_.widthAnchor constraintEqualToConstant:24],
    [spinner_.heightAnchor constraintEqualToConstant:24],
    [spinner_.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
    [caption_.leadingAnchor constraintEqualToAnchor:spinner_.trailingAnchor constant:12],
    [caption_.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-18],
    [caption_.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
    [self.heightAnchor constraintEqualToConstant:58],
  ]];
  return self;
}

- (void)setTargetLabel:(NSString*)label {
  caption_.stringValue = label.length ? [NSString stringWithFormat:@"Changing resolution to %@…", label]
                                      : @"Changing resolution…";
}

- (NSView*)hitTest:(NSPoint)point {
  return nil;
}

- (void)viewWillMoveToWindow:(NSWindow*)next {
  if (self.window) {
    [[NSNotificationCenter defaultCenter] removeObserver:self name:NSWindowWillCloseNotification
                                                  object:self.window];
  }
  [super viewWillMoveToWindow:next];
  if (next) {
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(windowWillClose:)
                                                 name:NSWindowWillCloseNotification object:next];
  }
}

- (void)viewDidMoveToWindow {
  [super viewDidMoveToWindow];
  if (self.window) {
    [spinner_ startAnimation:nil];
  } else {
    [spinner_ stopAnimation:nil];
  }
}

- (void)windowWillClose:(NSNotification*)notification {
  [self removeFromSuperview];
}

@end

namespace fm4_static_recomp::ui {
namespace {

NSWindow* GameWindow() {
  if (!SDL_WasInit(SDL_INIT_VIDEO)) return nil;
  SDL_Window* window = SDL_GetKeyboardFocus();
  if (!window) {
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    if (windows && count == 1) window = windows[0];
    SDL_free(windows);
  }
  if (!window) return nil;
  return (__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                    SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
}

// The game window owns the indicator. A closed window releases it, and late
// renderer notifications never retain a window or a drawable.
void UpdateLoadingView(NSWindow* game_window, NSString* target) {
  static NSHashTable<FM4ResolutionLoadingView*>* active =
      [[NSHashTable alloc] initWithOptions:NSPointerFunctionsWeakMemory capacity:1];
  NSView* content = game_window.contentView;
  FM4ResolutionLoadingView* current = nil;
  for (FM4ResolutionLoadingView* view in active.allObjects) {
    if (content && view.superview == content) {
      current = view;
    } else {
      [view removeFromSuperview];
      [active removeObject:view];
    }
  }
  if (!content) return;
  if (current) {
    [current setTargetLabel:target];
    return;
  }
  current = [[FM4ResolutionLoadingView alloc] initWithFrame:NSZeroRect];
  [current setTargetLabel:target];
  [content addSubview:current positioned:NSWindowAbove relativeTo:nil];
  NSLayoutConstraint* max_width = [current.widthAnchor constraintLessThanOrEqualToAnchor:content.widthAnchor];
  [NSLayoutConstraint activateConstraints:@[
    [current.centerXAnchor constraintEqualToAnchor:content.centerXAnchor],
    [current.centerYAnchor constraintEqualToAnchor:content.centerYAnchor],
    max_width,
  ]];
  [active addObject:current];
#if !__has_feature(objc_arc)
  [current release];
#endif
}

}  // namespace

void SetMacResolutionLoading(bool visible, std::string_view target_label) {
  static std::atomic<uint64_t> revision{0};
  const uint64_t requested = ++revision;
  const std::string label(target_label);
  auto apply = ^{
    if (requested != revision.load()) return;
    UpdateLoadingView(visible ? GameWindow() : nil, [NSString stringWithUTF8String:label.c_str()]);
  };
  if (NSThread.isMainThread) {
    apply();
  } else {
    dispatch_async(dispatch_get_main_queue(), apply);
  }
}

uint32_t CurrentMacWindowRefreshHz() {
  NSScreen* screen = GameWindow().screen;
  if (!screen) return 0;
  // A fixed mode such as 59.94 Hz takes precedence over the panel's maximum.
  // Variable-refresh modes can report zero. AppKit supplies their upper bound.
  NSNumber* display_id = [screen.deviceDescription objectForKey:@"NSScreenNumber"];
  if (display_id) {
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display_id.unsignedIntValue);
    if (mode) {
      const double hz = CGDisplayModeGetRefreshRate(mode);
      CGDisplayModeRelease(mode);
      if (hz > 1) return static_cast<uint32_t>(std::lround(hz));
    }
  }
  return static_cast<uint32_t>(screen.maximumFramesPerSecond);
}

}  // namespace fm4_static_recomp::ui
