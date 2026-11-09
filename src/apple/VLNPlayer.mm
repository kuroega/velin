// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Velin contributors
// Objective-C facade over velin::MediaEngine. Built with ARC.
#import "velin/apple/VLNPlayer.h"

#include "velin/alist.hpp"
#include "velin/media_engine.hpp"
#include "velin/system.hpp"

#include <memory>
#include <string>
#include <vector>

#if !__has_feature(objc_arc)
#error "VLNPlayer.mm must be compiled with -fobjc-arc"
#endif

namespace {
NSString* ns(const std::string& text) {
    NSString* result = [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding];
    return result ? result : @"";
}
std::string utf8(NSString* text) {
    const char* bytes = text.UTF8String;
    return bytes ? std::string(bytes) : std::string{};
}
std::string mediaPath(NSURL* url) {
    return url.isFileURL ? utf8(url.path) : utf8(url.absoluteString);
}
} // namespace

@implementation VLNMediaItem
- (instancetype)initWithURL:(NSURL*)url title:(NSString*)title {
    if ((self = [super init])) {
        _url = url;
        _title = [title copy];
        _historyKey = url.isFileURL ? url.path : url.absoluteString;
        _subtitleURLs = @[];
        _subtitleNames = @[];
    }
    return self;
}
@end

@interface VLNPlaylistEntry ()
- (instancetype)initWithTitle:(NSString*)title current:(BOOL)current;
@end

@implementation VLNPlaylistEntry
- (instancetype)initWithTitle:(NSString*)title current:(BOOL)current {
    if ((self = [super init])) { _title = [title copy]; _current = current; }
    return self;
}
@end

@interface VLNTrack ()
- (instancetype)initWithTrack:(const velin::Track&)track;
@end

@implementation VLNTrack
- (instancetype)initWithTrack:(const velin::Track&)track {
    if ((self = [super init])) {
        _identifier = static_cast<NSInteger>(track.id);
        _type = ns(track.type);
        _title = ns(track.title);
        _language = ns(track.language);
        _codec = ns(track.codec);
        _selected = track.selected;
    }
    return self;
}
@end

@implementation VLNPlayer {
    std::unique_ptr<velin::MediaEngine> _engine;
    CAMetalLayer* _layer;  // mpv holds a raw pointer to it while running.
    // Items from -openItems:, by the path string mpv reports for them.
    NSMutableDictionary<NSString*, VLNMediaItem*>* _items;
    std::string _appliedPath;  // The loaded file whose subtitles and resume are applied.
    NSUInteger _resizeGeneration;  // Coalesces -layerDidResize calls.
    BOOL _resizeFilterOn;
    std::string _suspendedVid;  // The vid option (JSON) to restore after -setVideoSuspended:NO.
}

- (instancetype)init {
    if ((self = [super init])) {
        _engine = std::make_unique<velin::MediaEngine>();
        _items = [NSMutableDictionary dictionary];
        _subtitleLanguages = @"";
    }
    return self;
}

- (void)dealloc { _engine->shutdown(); }

- (BOOL)startWithLayer:(CAMetalLayer*)layer options:(NSDictionary<NSString*, NSString*>*)options error:(NSError**)error {
    velin::EngineConfiguration configuration;
    if (layer) {
        // Vulkan through MoltenVK into the layer, with VideoToolbox decoding.
        configuration.options = {{"vo", "gpu-next"}, {"gpu-api", "vulkan"}, {"gpu-context", "moltenvk"}, {"hwdec", "videotoolbox"}};
    }
    // The app resumes from Velin's history (VLNMediaItem.resumePosition),
    // which also covers AList files whose signed URLs change.
    configuration.options["resume-playback"] = "no";
    configuration.options["save-position-on-quit"] = "no";
    for (NSString* key in options) configuration.options[utf8(key)] = utf8(options[key]);
    __weak VLNPlayer* weakSelf = self;
    auto wakeup = [weakSelf]() -> bool {
        dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf drainEvents]; });
        return true;
    };
    _layer = layer;
    const velin::VideoOutput output{reinterpret_cast<std::intptr_t>((__bridge void*)layer)};
    if (!_engine->initialize(output, wakeup, configuration)) {
        _layer = nil;
        if (error) {
            *error = [NSError errorWithDomain:@"app.velin.player" code:1
                                     userInfo:@{NSLocalizedDescriptionKey: ns(_engine->error())}];
        }
        return NO;
    }
    return YES;
}

- (void)shutdown {
    _engine->shutdown();
    _layer = nil;
}

- (BOOL)isRunning { return _engine->ready(); }

// The MoltenVK context in mpv 0.41 has no resize event: it reads the layer's
// drawableSize only in reconfig, which runs when the video's parameters
// change (see mpvkit/MPVKit#3). Adding or removing a filter that only marks
// the video as stereo changes those parameters without changing the picture
// (gpu-next ignores the stereo mode), so each toggle makes mpv reconfigure
// once at the layer's current size. mpv refreshes the frame itself when paused.
- (void)layerDidResize {
    const NSUInteger generation = ++_resizeGeneration;
    __weak VLNPlayer* weakSelf = self;
    // Wait for the rotation's layout passes to settle.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 150 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        VLNPlayer* strongSelf = weakSelf;
        if (strongSelf && strongSelf->_resizeGeneration == generation) [strongSelf reconfigureVideoOutput];
    });
}

- (void)reconfigureVideoOutput {
    // A file loaded later reconfigures at the then-current size anyway.
    if (!_layer || !_engine->ready() || !_engine->state().loaded) return;
    const bool ok = _resizeFilterOn ? _engine->command({"vf", "remove", "@velin-resize"})
                                    : _engine->command({"vf", "add", "@velin-resize:format=stereo-in=ab2l"});
    if (ok) _resizeFilterOn = !_resizeFilterOn;
}

// mpv's iOS advice: no video track while in the background. Setting vid
// synchronously tears the VO (and its VkDevice) down before the app is
// suspended, and the option also keeps files loaded in the background from
// opening a new one. The option, not the current track, is saved: it is
// "auto" or the track the user picked, and setting it back reselects the
// track and makes mpv decode the current frame again, also when paused.
- (void)setVideoSuspended:(BOOL)suspended {
    if (suspended == _videoSuspended || !_layer || !_engine->ready()) return;
    std::string error;
    if (suspended) {
        auto vid = _engine->propertyJson("options/vid");
        if (vid == "null") vid = "\"auto\"";
        if (!_engine->setPropertyJsonChecked("vid", "\"no\"", error)) {
            NSLog(@"VELIN cannot suspend video: %s", error.c_str());
            return;
        }
        _suspendedVid = vid;
        _videoSuspended = YES;
    } else {
        _videoSuspended = NO;
        if (!_engine->setPropertyJsonChecked("vid", _suspendedVid, error)) {
            NSLog(@"VELIN cannot resume video: %s", error.c_str());
        }
    }
}

- (void)drainEvents {
    if (!_engine->ready()) return;
    _engine->poll();
    [self applyItemSettings];
    [self.delegate playerDidUpdate:self];
}

// When an item from -openItems: finishes loading, attach its subtitles and
// resume it, once per load.
- (void)applyItemSettings {
    const auto& state = _engine->state();
    if (state.idle) { _appliedPath.clear(); return; }
    if (!state.loaded || state.path == _appliedPath) return;
    _appliedPath = state.path;
    VLNMediaItem* item = _items[ns(state.path)];
    if (!item) return;
    std::vector<std::string> names;
    for (NSString* name in item.subtitleNames) names.push_back(utf8(name));
    const auto selected = velin::alist::preferredSubtitle(names, utf8(self.subtitleLanguages));
    for (NSUInteger i = 0; i < item.subtitleURLs.count; ++i) {
        const std::string name = i < names.size() ? names[i] : std::string{};
        _engine->command({"sub-add", mediaPath(item.subtitleURLs[i]), i == selected ? "select" : "auto", name});
    }
    if (item.resumePosition > 1) {
        _engine->seek(item.resumePosition, true);
        item.resumePosition = 0;  // Replaying the file later starts over.
    }
}

- (BOOL)openItems:(NSArray<VLNMediaItem*>*)items startIndex:(NSInteger)startIndex {
    if (items.count == 0 || startIndex < 0 || startIndex >= static_cast<NSInteger>(items.count)) return NO;
    [_items removeAllObjects];
    _appliedPath.clear();
    std::vector<std::string> before, after;
    for (NSInteger i = 0; i < static_cast<NSInteger>(items.count); ++i) {
        const auto path = mediaPath(items[i].url);
        _items[ns(path)] = items[i];
        if (i < startIndex) before.push_back(path);
        else if (i > startIndex) after.push_back(path);
    }
    // The chosen file first, then the ones after it, then the earlier ones
    // moved to the front: the same order the Windows player uses.
    if (!_engine->open({mediaPath(items[startIndex].url)}, false)) return NO;
    if (!after.empty()) _engine->open(after, true);
    if (!before.empty()) {
        _engine->open(before, true);
        const auto total = static_cast<std::int64_t>(items.count), earlier = static_cast<std::int64_t>(before.size());
        for (std::int64_t i = 0; i < earlier; ++i) _engine->movePlaylist(total - earlier + i, i);
    }
    return YES;
}

- (VLNMediaItem*)currentItem {
    const auto& path = _engine->state().path;
    return path.empty() ? nil : _items[ns(path)];
}

- (NSArray<VLNPlaylistEntry*>*)playlist {
    NSMutableArray<VLNPlaylistEntry*>* result = [NSMutableArray array];
    for (const auto& entry : _engine->state().playlist) {
        VLNMediaItem* item = _items[ns(entry.filename)];
        NSString* title = item ? item.title : ns(entry.title);
        if (!item && entry.title.empty()) {
            NSString* name = ns(entry.filename).lastPathComponent;
            title = name.stringByRemovingPercentEncoding ? name.stringByRemovingPercentEncoding : name;
        }
        [result addObject:[[VLNPlaylistEntry alloc] initWithTitle:title current:entry.current]];
    }
    return result;
}

- (NSInteger)playlistPosition { return static_cast<NSInteger>(_engine->state().playlistPosition); }
- (void)selectPlaylistIndex:(NSInteger)index { _engine->selectPlaylist(index); }
- (void)playNext { _engine->command({"playlist-next"}); }
- (void)playPrevious { _engine->command({"playlist-prev"}); }

- (BOOL)openURLs:(NSArray<NSURL*>*)urls append:(BOOL)append {
    std::vector<std::string> paths;
    for (NSURL* url in urls) paths.push_back(mediaPath(url));
    return _engine->open(paths, append);
}

- (void)togglePause { _engine->togglePause(); }
- (void)seekTo:(double)seconds { _engine->seek(seconds, true); }
- (void)seekBy:(double)seconds { _engine->seek(seconds, false); }
- (void)stop { _engine->stop(); }

- (void)selectTrack:(VLNTrack*)track {
    NSString* property = [track.type isEqualToString:@"sub"] ? @"sid" : [track.type isEqualToString:@"audio"] ? @"aid" : @"vid";
    _engine->setProperty(utf8(property), std::to_string(track.identifier));
}

- (BOOL)setProperty:(NSString*)name value:(NSString*)value { return _engine->setProperty(utf8(name), utf8(value)); }

- (NSString*)propertyJSON:(NSString*)name {
    const auto json = _engine->propertyJson(utf8(name));
    return json.empty() ? nil : ns(json);
}

- (BOOL)isIdle { return _engine->state().idle; }
- (BOOL)isLoaded { return _engine->state().loaded; }
- (BOOL)isPaused { return _engine->state().paused; }
- (double)position { return _engine->state().position; }
- (double)duration { return _engine->state().duration; }
- (NSString*)title { return ns(_engine->state().title); }
- (NSString*)errorMessage { return ns(_engine->state().error.empty() ? _engine->error() : _engine->state().error); }
- (NSString*)videoCodec { return ns(_engine->state().videoCodec); }
- (NSString*)audioCodec { return ns(_engine->state().audioCodec); }
- (NSString*)hardwareDecoder { return ns(_engine->state().hardwareDecoder); }
- (NSInteger)videoWidth { return static_cast<NSInteger>(_engine->state().width); }
- (NSInteger)videoHeight { return static_cast<NSInteger>(_engine->state().height); }

- (double)speed { return _engine->state().speed; }
- (double)volume { return _engine->state().volume; }

- (NSArray<VLNTrack*>*)tracks {
    NSMutableArray<VLNTrack*>* result = [NSMutableArray array];
    for (const auto& track : _engine->state().tracks) [result addObject:[[VLNTrack alloc] initWithTrack:track]];
    return result;
}

@end

@implementation VLNDiagnostics
+ (NSArray<NSString*>*)runSecretStoreCheck {
    NSMutableArray<NSString*>* lines = [NSMutableArray array];
    try {
        const std::string secret = "velin secret-store check";
        const auto sealed = velin::sys::protectSecret(secret, "Velin diagnostics");
        const bool hidden = sealed.find(secret) == std::string::npos;
        const bool roundTrip = velin::sys::unprotectSecret(sealed) == secret;
        auto tampered = sealed;
        tampered.back() = static_cast<char>(tampered.back() ^ 1);
        bool rejected = false;
        try { velin::sys::unprotectSecret(tampered); } catch (const std::exception&) { rejected = true; }
        [lines addObject:hidden && roundTrip ? @"PASS Keychain secret round trip" : @"FAIL Keychain secret round trip"];
        [lines addObject:rejected ? @"PASS tampered secret rejected" : @"FAIL tampered secret accepted"];
    } catch (const std::exception& error) {
        [lines addObject:[@"FAIL secret store: " stringByAppendingString:ns(error.what())]];
    }
    return lines;
}
@end
