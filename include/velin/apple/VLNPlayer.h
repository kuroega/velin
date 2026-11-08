// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Velin contributors
//
// The Swift-facing face of Velin's playback engine on Apple platforms. Plain
// Objective-C, so Swift imports it without C++ interop. Use from the main
// thread only.
#import <Foundation/Foundation.h>
#import <QuartzCore/CAMetalLayer.h>

NS_ASSUME_NONNULL_BEGIN

@class VLNPlayer;

@protocol VLNPlayerDelegate <NSObject>
// Called on the main thread after the engine reports changes.
- (void)playerDidUpdate:(VLNPlayer *)player;
@end

@interface VLNTrack : NSObject
@property (nonatomic, readonly) NSInteger identifier;
@property (nonatomic, readonly, copy) NSString *type;  // "video", "audio" or "sub".
@property (nonatomic, readonly, copy) NSString *title;
@property (nonatomic, readonly, copy) NSString *language;
@property (nonatomic, readonly, copy) NSString *codec;
@property (nonatomic, readonly, getter=isSelected) BOOL selected;
@end

@interface VLNPlayer : NSObject

@property (nonatomic, weak, nullable) id<VLNPlayerDelegate> delegate;

// Starts the engine. mpv renders into the layer through Vulkan (MoltenVK);
// pass nil for audio-only playback. The player keeps the layer alive until
// -shutdown. Extra mpv options override Velin's defaults.
- (BOOL)startWithLayer:(nullable CAMetalLayer *)layer
               options:(nullable NSDictionary<NSString *, NSString *> *)options
                 error:(NSError **)error;
- (void)shutdown;
@property (nonatomic, readonly, getter=isRunning) BOOL running;

// Local files or http(s) URLs.
- (BOOL)openURLs:(NSArray<NSURL *> *)urls append:(BOOL)append;
- (void)togglePause;
- (void)seekTo:(double)seconds;
- (void)seekBy:(double)seconds;
- (void)stop;
- (void)selectTrack:(VLNTrack *)track;
- (BOOL)setProperty:(NSString *)name value:(NSString *)value;
// Any mpv property as JSON, or nil when unavailable.
- (nullable NSString *)propertyJSON:(NSString *)name;

@property (nonatomic, readonly, getter=isIdle) BOOL idle;
@property (nonatomic, readonly, getter=isLoaded) BOOL loaded;
@property (nonatomic, readonly, getter=isPaused) BOOL paused;
@property (nonatomic, readonly) double position;
@property (nonatomic, readonly) double duration;
@property (nonatomic, readonly, copy) NSString *title;
@property (nonatomic, readonly, copy) NSString *errorMessage;
@property (nonatomic, readonly, copy) NSString *videoCodec;
@property (nonatomic, readonly, copy) NSString *audioCodec;
@property (nonatomic, readonly, copy) NSString *hardwareDecoder;
@property (nonatomic, readonly) NSInteger videoWidth;
@property (nonatomic, readonly) NSInteger videoHeight;
@property (nonatomic, readonly, copy) NSArray<VLNTrack *> *tracks;

@end

// Checks that platform services work in this build: the Keychain-backed
// secret store and HTTPS. Returns one line per check, prefixed PASS or FAIL.
@interface VLNDiagnostics : NSObject
+ (NSArray<NSString *> *)runSecretStoreCheck;
@end

NS_ASSUME_NONNULL_END
