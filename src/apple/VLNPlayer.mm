// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Velin contributors
// Objective-C facade over velin::MediaEngine. Built with ARC.
#import "velin/apple/VLNPlayer.h"

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
}

- (instancetype)init {
    if ((self = [super init])) _engine = std::make_unique<velin::MediaEngine>();
    return self;
}

- (void)dealloc { _engine->shutdown(); }

- (BOOL)startWithLayer:(CAMetalLayer*)layer options:(NSDictionary<NSString*, NSString*>*)options error:(NSError**)error {
    velin::EngineConfiguration configuration;
    if (layer) {
        // Vulkan through MoltenVK into the layer, with VideoToolbox decoding.
        configuration.options = {{"vo", "gpu-next"}, {"gpu-api", "vulkan"}, {"gpu-context", "moltenvk"}, {"hwdec", "videotoolbox"}};
    }
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

- (void)drainEvents {
    if (!_engine->ready()) return;
    _engine->poll();
    [self.delegate playerDidUpdate:self];
}

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
