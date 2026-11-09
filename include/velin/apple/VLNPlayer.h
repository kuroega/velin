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

// Something to play: a local file or URL, plus what Velin knows about it.
@interface VLNMediaItem : NSObject
- (instancetype)initWithURL:(NSURL *)url title:(NSString *)title NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;
@property (nonatomic, readonly) NSURL *url;
@property (nonatomic, readonly, copy) NSString *title;
// Stable identity for history and resume, e.g. "velin://alist?path=..." for an
// AList file whose signed URL may change. Defaults to the URL.
@property (nonatomic, copy) NSString *historyKey;
// Seconds to resume from once the file loads; 0 starts at the beginning.
@property (nonatomic) double resumePosition;
// External subtitles attached when this item starts playing.
@property (nonatomic, copy) NSArray<NSURL *> *subtitleURLs;
@property (nonatomic, copy) NSArray<NSString *> *subtitleNames;
@end

@interface VLNPlaylistEntry : NSObject
@property (nonatomic, readonly, copy) NSString *title;
@property (nonatomic, readonly, getter=isCurrent) BOOL current;
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

// Call after the layer's drawableSize changes (rotation, split view). mpv's
// MoltenVK backend reads that size only when the video output reconfigures,
// so without this it keeps drawing at the old size. Calls are coalesced.
- (void)layerDidResize;

// iOS forbids GPU work from the background: a frame submitted then loses the
// Vulkan device for good. Suspend video when the app enters the background
// and resume it once the app is active again. Suspending deselects the video track
// (vid=no), which closes mpv's video output before this returns; audio keeps
// playing. Resuming restores the earlier selection and redraws the frame.
@property (nonatomic, getter=isVideoSuspended) BOOL videoSuspended;

// Local files or http(s) URLs.
- (BOOL)openURLs:(NSArray<NSURL *> *)urls append:(BOOL)append;
// Replaces the playlist with items and starts at items[startIndex]. The chosen
// item loads first, so nothing else starts streaming before it.
- (BOOL)openItems:(NSArray<VLNMediaItem *> *)items startIndex:(NSInteger)startIndex;
// The item playing now, when it was opened with -openItems:startIndex:.
@property (nonatomic, readonly, nullable) VLNMediaItem *currentItem;
@property (nonatomic, readonly, copy) NSArray<VLNPlaylistEntry *> *playlist;
@property (nonatomic, readonly) NSInteger playlistPosition;
- (void)selectPlaylistIndex:(NSInteger)index;
- (void)playNext;
- (void)playPrevious;
// Subtitle languages in mpv's slang form, e.g. "zh-Hans,chi,eng", used to pick
// among an item's external subtitles.
@property (nonatomic, copy) NSString *subtitleLanguages;
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
@property (nonatomic, readonly) double speed;
@property (nonatomic, readonly) double volume;  // 0–100

@end

// ---- Playback history (Velin's HistoryStore) -------------------------------

@interface VLNHistoryEntry : NSObject
@property (nonatomic, readonly, copy) NSString *key;
@property (nonatomic, readonly, copy) NSString *title;
@property (nonatomic, readonly) NSDate *lastPlayed;
@property (nonatomic, readonly) double position;
@property (nonatomic, readonly) double duration;
@property (nonatomic, readonly, getter=isFavorite) BOOL favorite;
@end

@interface VLNLibrary : NSObject
// History lives in Velin's data folder (Application Support/Velin).
- (instancetype)init;
// Most recent first.
- (NSArray<VLNHistoryEntry *> *)recentEntries;
- (nullable VLNHistoryEntry *)entryForKey:(NSString *)key;
- (void)recordKey:(NSString *)key title:(NSString *)title position:(double)position
         duration:(double)duration countPlay:(BOOL)countPlay;
- (void)removeKey:(NSString *)key;
- (void)setFavorite:(BOOL)favorite forKey:(NSString *)key;
- (void)clear;
@end

// ---- AList -------------------------------------------------------------------

FOUNDATION_EXPORT NSErrorDomain const VLNAListErrorDomain;
typedef NS_ERROR_ENUM(VLNAListErrorDomain, VLNAListErrorCode) {
    VLNAListErrorInvalidServer = 1,
    VLNAListErrorTransport,
    VLNAListErrorInvalidResponse,
    VLNAListErrorUnauthorized,   // Sign in again.
    VLNAListErrorOtpRequired,    // Ask for a two-factor code.
    VLNAListErrorForbidden,      // No permission or wrong folder password.
    VLNAListErrorRateLimited,
    VLNAListErrorServer,
};

@interface VLNAListEntry : NSObject
@property (nonatomic, readonly, copy) NSString *name;
@property (nonatomic, readonly, copy) NSString *path;
@property (nonatomic, readonly, getter=isDirectory) BOOL directory;
@property (nonatomic, readonly, getter=isPlayable) BOOL playable;
@property (nonatomic, readonly) long long size;
@property (nonatomic, readonly, copy) NSString *modified;  // RFC 3339.
@end

@interface VLNAListFolder : NSObject
@property (nonatomic, readonly, copy) NSString *path;
// Folders first, then files, each in natural order.
@property (nonatomic, readonly, copy) NSArray<VLNAListEntry *> *entries;
@end

// "velin://alist?path=<AList path>", the link the Windows player opens too.
FOUNDATION_EXPORT NSString *VLNAListLink(NSString *path);

// Signs in to one AList server and browses it. Requests run in the
// background; completion blocks run on the main thread. The sign-in is saved
// encrypted (Keychain key) and restored on launch.
@interface VLNAList : NSObject
@property (nonatomic, readonly, getter=isSignedIn) BOOL signedIn;
@property (nonatomic, readonly, copy) NSString *server;    // Empty before the first sign-in.
@property (nonatomic, readonly, copy) NSString *username;
@property (nonatomic, readonly, copy) NSString *lastFolder; // "/" when unknown.
- (void)signInToServer:(NSString *)server username:(NSString *)username password:(NSString *)password
               otpCode:(nullable NSString *)otpCode completion:(void (^)(NSError *_Nullable error))completion;
- (void)signOut;
// Remembers a folder password for the session (it also covers subfolders).
- (void)setPassword:(NSString *)password forFolder:(NSString *)path;
- (void)listFolder:(NSString *)path refresh:(BOOL)refresh
        completion:(void (^)(VLNAListFolder *_Nullable folder, NSError *_Nullable error))completion;
// Every playable file in the folder, each with its subtitle siblings, and the
// index of path. Nil when path is not a playable file there.
- (nullable NSArray<VLNMediaItem *> *)playlistForFolder:(VLNAListFolder *)folder
                                             startingAt:(NSString *)path
                                             startIndex:(NSInteger *)startIndex;
@end

// Checks that platform services work in this build: the Keychain-backed
// secret store and HTTPS. Returns one line per check, prefixed PASS or FAIL.
@interface VLNDiagnostics : NSObject
+ (NSArray<NSString *> *)runSecretStoreCheck;
@end

NS_ASSUME_NONNULL_END
