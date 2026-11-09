// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Velin contributors
// Objective-C face of velin::HistoryStore. Built with ARC.
#import "velin/apple/VLNPlayer.h"

#include "velin/library.hpp"
#include "velin/system.hpp"

#include <string>

namespace {
NSString* ns(const std::string& text) {
    NSString* result = [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding];
    return result ? result : @"";
}
std::string utf8(NSString* text) {
    const char* bytes = text.UTF8String;
    return bytes ? std::string(bytes) : std::string{};
}
} // namespace

@interface VLNHistoryEntry ()
- (instancetype)initWithEntry:(const velin::HistoryEntry&)entry;
@end

@implementation VLNHistoryEntry
- (instancetype)initWithEntry:(const velin::HistoryEntry&)entry {
    if ((self = [super init])) {
        _key = ns(entry.path);
        _title = ns(entry.title);
        _lastPlayed = [NSDate dateWithTimeIntervalSince1970:static_cast<NSTimeInterval>(entry.lastPlayed)];
        _position = entry.position;
        _duration = entry.duration;
        _favorite = entry.favorite;
    }
    return self;
}
@end

@implementation VLNLibrary {
    velin::HistoryStore* _store;
}

- (instancetype)init {
    if ((self = [super init])) _store = new velin::HistoryStore(velin::sys::dataDirectory());
    return self;
}

- (void)dealloc { delete _store; }

- (NSArray<VLNHistoryEntry*>*)recentEntries {
    NSMutableArray<VLNHistoryEntry*>* result = [NSMutableArray array];
    for (const auto& entry : _store->search("", velin::HistorySort::Recent))
        [result addObject:[[VLNHistoryEntry alloc] initWithEntry:entry]];
    return result;
}

- (VLNHistoryEntry*)entryForKey:(NSString*)key {
    const auto wanted = utf8(key);
    for (const auto& entry : _store->load())
        if (entry.path == wanted) return [[VLNHistoryEntry alloc] initWithEntry:entry];
    return nil;
}

- (void)recordKey:(NSString*)key title:(NSString*)title position:(double)position
         duration:(double)duration countPlay:(BOOL)countPlay {
    velin::HistoryEntry entry;
    entry.path = utf8(key);
    entry.title = utf8(title);
    entry.position = position > 0 ? position : 0;
    entry.duration = duration > 0 ? duration : 0;
    if (!_store->record(entry, countPlay)) NSLog(@"Velin history: %s", _store->error().c_str());
}

- (void)removeKey:(NSString*)key { _store->remove(utf8(key)); }
- (void)setFavorite:(BOOL)favorite forKey:(NSString*)key { _store->setFavorite(utf8(key), favorite); }
- (void)clear { _store->clear(false); }
@end
