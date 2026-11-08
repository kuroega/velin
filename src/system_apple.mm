// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Velin contributors
// Apple (iOS and macOS) implementation of velin/system.hpp. Built with ARC.
#include "velin/system.hpp"

#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include <CommonCrypto/CommonCrypto.h>
#include <CoreFoundation/CoreFoundation.h>
#include <TargetConditionals.h>
#include <mach-o/dyld.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>

#if TARGET_OS_OSX
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#if !__has_feature(objc_arc)
#error "system_apple.mm must be compiled with -fobjc-arc"
#endif

namespace {
void ensure(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
NSString* toNSString(std::string_view text) {
    NSString* result = [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding];
    ensure(result != nil, "Text is not valid UTF-8");
    return result;
}
std::string fromNSString(NSString* text) {
    if (!text) return {};
    const char* bytes = text.UTF8String;
    return bytes ? std::string(bytes) : std::string{};
}
} // namespace

// ---- HTTP -----------------------------------------------------------------

@interface VelinHttpDelegate : NSObject <NSURLSessionDataDelegate>
@end

@implementation VelinHttpDelegate {
@public
    std::string body;
    std::string location;
    std::size_t maximum;
    unsigned status;
    bool tooLarge;
    NSError* failure;
    dispatch_semaphore_t done;
}

- (instancetype)init {
    if ((self = [super init])) { maximum = 0; status = 0; tooLarge = false; done = dispatch_semaphore_create(0); }
    return self;
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task
    willPerformHTTPRedirection:(NSHTTPURLResponse*)response newRequest:(NSURLRequest*)request
    completionHandler:(void (^)(NSURLRequest*))completionHandler {
    completionHandler(nil);  // Velin follows redirects itself, applying its own policy.
}

- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)dataTask
    didReceiveResponse:(NSURLResponse*)response
    completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
    if (auto* http = [response isKindOfClass:[NSHTTPURLResponse class]] ? (NSHTTPURLResponse*)response : nil) {
        status = static_cast<unsigned>(http.statusCode);
        location = fromNSString([http valueForHTTPHeaderField:@"Location"]);
    }
    completionHandler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)dataTask didReceiveData:(NSData*)data {
    if (tooLarge) return;
    if (body.size() + data.length > maximum) { tooLarge = true; [dataTask cancel]; return; }
    [data enumerateByteRangesUsingBlock:^(const void* bytes, NSRange range, BOOL*) {
        body.append(static_cast<const char*>(bytes), range.length);
    }];
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error {
    failure = error;
    dispatch_semaphore_signal(done);
}
@end

namespace velin::sys {

HttpExchangeResponse httpExchange(const HttpExchangeRequest& request, std::stop_token stop) {
    NSURL* url = [NSURL URLWithString:toNSString(request.url)];
    ensure(url != nil, "Invalid HTTP URL");
    NSMutableURLRequest* native = [NSMutableURLRequest requestWithURL:url];
    native.HTTPMethod = toNSString(request.method);
    native.cachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    native.HTTPShouldHandleCookies = NO;
    native.timeoutInterval = request.timeoutMilliseconds / 1000.0;
    bool userAgent = false;
    for (const auto& [key, value] : request.headers) {
        [native setValue:toNSString(value) forHTTPHeaderField:toNSString(key)];
        userAgent = userAgent || strcasecmp(key.c_str(), "User-Agent") == 0;
    }
    if (!userAgent) [native setValue:@"Velin/" VELIN_VERSION forHTTPHeaderField:@"User-Agent"];
    if (!request.body.empty()) native.HTTPBody = [NSData dataWithBytes:request.body.data() length:request.body.size()];

    NSURLSessionConfiguration* configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    configuration.URLCache = nil;
    configuration.HTTPCookieStorage = nil;
    configuration.timeoutIntervalForRequest = request.timeoutMilliseconds / 1000.0;
    VelinHttpDelegate* delegate = [VelinHttpDelegate new];
    delegate->maximum = request.maximumBytes;
    NSOperationQueue* queue = [NSOperationQueue new];
    queue.maxConcurrentOperationCount = 1;
    NSURLSession* session = [NSURLSession sessionWithConfiguration:configuration delegate:delegate delegateQueue:queue];
    NSURLSessionDataTask* task = [session dataTaskWithRequest:native];
    {
        std::stop_callback cancel(stop, [task] { [task cancel]; });
        [task resume];
        dispatch_semaphore_wait(delegate->done, DISPATCH_TIME_FOREVER);
    }
    [session invalidateAndCancel];  // Releases the delegate.
    if (stop.stop_requested()) throw std::runtime_error("Cancelled");
    ensure(!delegate->tooLarge, "HTTP response exceeded its size limit");
    const bool redirect = delegate->status == 301 || delegate->status == 302 || delegate->status == 303 ||
                          delegate->status == 307 || delegate->status == 308;
    // A refused redirect completes normally; any other error is a transport failure.
    if (delegate->failure && !(redirect && delegate->failure.code == NSURLErrorCancelled))
        throw std::runtime_error(fromNSString(delegate->failure.localizedDescription));
    ensure(delegate->status != 0, "Cannot read HTTP status");
    HttpExchangeResponse result;
    result.status = delegate->status;
    if (redirect) {
        ensure(!delegate->location.empty() && delegate->location.size() < 32768, "Invalid HTTP redirect");
        result.location = std::move(delegate->location);
    } else {
        result.body = std::move(delegate->body);
    }
    return result;
}

// ---- Cryptography ---------------------------------------------------------

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
std::array<unsigned char, 20> sha1(std::string_view data) {
    // SHA-1 is required by RFC 6455 for the WebSocket handshake, not for security.
    std::array<unsigned char, 20> digest{};
    CC_SHA1(data.data(), static_cast<CC_LONG>(data.size()), digest.data());
    return digest;
}
#pragma clang diagnostic pop

std::array<unsigned char, 32> sha256(std::string_view data) {
    std::array<unsigned char, 32> digest{};
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    for (std::size_t offset = 0; offset < data.size(); offset += 1u << 30) {
        const auto chunk = std::min<std::size_t>(data.size() - offset, 1u << 30);
        CC_SHA256_Update(&context, data.data() + offset, static_cast<CC_LONG>(chunk));
    }
    CC_SHA256_Final(digest.data(), &context);
    return digest;
}

std::array<unsigned char, 32> sha256File(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    ensure(static_cast<bool>(input), "Cannot read file for hashing");
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    char buffer[65536];
    while (input.read(buffer, sizeof(buffer)) || input.gcount())
        CC_SHA256_Update(&context, buffer, static_cast<CC_LONG>(input.gcount()));
    ensure(input.eof(), "File read failed during hashing");
    std::array<unsigned char, 32> digest{};
    CC_SHA256_Final(digest.data(), &context);
    return digest;
}

void randomBytes(void* buffer, std::size_t size) {
    ensure(SecRandomCopyBytes(kSecRandomDefault, size, buffer) == errSecSuccess, "Cannot generate random bytes");
}

void secureZero(void* buffer, std::size_t size) {
    // Volatile stores keep the compiler from dropping a "dead" wipe.
    auto* bytes = static_cast<volatile unsigned char*>(buffer);
    for (std::size_t i = 0; buffer && i < size; ++i) bytes[i] = 0;
}

// Secrets are sealed with AES-256-CBC and HMAC-SHA256 (encrypt-then-MAC) under
// a random 512-bit key held in the Keychain, readable only on this device
// after its first unlock.
namespace {
constexpr char kSecretMagic[] = "VLK1";
constexpr std::size_t kMagicSize = 4, kIvSize = kCCBlockSizeAES128, kMacSize = CC_SHA256_DIGEST_LENGTH;

NSDictionary* keyQuery() {
    return @{
        (__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
        (__bridge id)kSecAttrService: @"app.velin.secrets",
        (__bridge id)kSecAttrAccount: @"secret-key-v1",
        (__bridge id)kSecUseDataProtectionKeychain: @YES,
    };
}

std::string keychainError(OSStatus status) {
    if (status == errSecMissingEntitlement)
        return "The Keychain is unavailable to this unsigned build (errSecMissingEntitlement)";
    return "Keychain error " + std::to_string(static_cast<int>(status));
}

std::string secretKey() {
    NSMutableDictionary* query = [keyQuery() mutableCopy];
    query[(__bridge id)kSecReturnData] = @YES;
    query[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
    for (int attempt = 0; attempt < 2; ++attempt) {
        CFTypeRef found = nullptr;
        OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef)query, &found);
        if (status == errSecSuccess) {
            NSData* data = (__bridge_transfer NSData*)found;
            ensure(data.length == 64, "The Velin Keychain key is damaged");
            return std::string(static_cast<const char*>(data.bytes), data.length);
        }
        ensure(status == errSecItemNotFound, keychainError(status));
        std::string key(64, '\0');
        randomBytes(key.data(), key.size());
        NSMutableDictionary* item = [keyQuery() mutableCopy];
        item[(__bridge id)kSecValueData] = [NSData dataWithBytes:key.data() length:key.size()];
        item[(__bridge id)kSecAttrAccessible] = (__bridge id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly;
        item[(__bridge id)kSecAttrLabel] = @"Velin secret storage key";
        status = SecItemAdd((__bridge CFDictionaryRef)item, nullptr);
        if (status == errSecSuccess) return key;
        secureZero(key.data(), key.size());
        ensure(status == errSecDuplicateItem, keychainError(status));  // Lost a race; read the winner.
    }
    throw std::runtime_error("Cannot create the Velin Keychain key");
}

std::array<unsigned char, kMacSize> mac(const std::string& key, std::string_view data) {
    std::array<unsigned char, kMacSize> result{};
    CCHmac(kCCHmacAlgSHA256, key.data() + 32, 32, data.data(), data.size(), result.data());
    return result;
}
} // namespace

std::string protectSecret(std::string_view plain, std::string_view) {
    auto key = secretKey();
    std::string result(kSecretMagic, kMagicSize);
    result.resize(kMagicSize + kIvSize);
    randomBytes(result.data() + kMagicSize, kIvSize);
    std::string cipher(plain.size() + kCCBlockSizeAES128, '\0');
    std::size_t written = 0;
    const auto status = CCCrypt(kCCEncrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding, key.data(), 32,
        result.data() + kMagicSize, plain.data(), plain.size(), cipher.data(), cipher.size(), &written);
    if (status != kCCSuccess) { secureZero(key.data(), key.size()); throw std::runtime_error("Cannot encrypt the secret"); }
    result.append(cipher.data(), written);
    const auto tag = mac(key, result);
    result.append(reinterpret_cast<const char*>(tag.data()), tag.size());
    secureZero(key.data(), key.size());
    return result;
}

std::string unprotectSecret(std::string_view protectedBytes) {
    ensure(protectedBytes.size() >= kMagicSize + kIvSize + kCCBlockSizeAES128 + kMacSize &&
           protectedBytes.substr(0, kMagicSize) == std::string_view(kSecretMagic, kMagicSize), "Cannot decrypt the secret: unknown format");
    auto key = secretKey();
    const auto sealed = protectedBytes.substr(0, protectedBytes.size() - kMacSize);
    const auto expected = mac(key, sealed);
    if (timingsafe_bcmp(expected.data(), protectedBytes.data() + sealed.size(), kMacSize) != 0) {
        secureZero(key.data(), key.size());
        throw std::runtime_error("Cannot decrypt the secret: it was modified or belongs to another device");
    }
    const auto cipher = sealed.substr(kMagicSize + kIvSize);
    std::string plain(cipher.size(), '\0');
    std::size_t written = 0;
    const auto status = CCCrypt(kCCDecrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding, key.data(), 32,
        sealed.data() + kMagicSize, cipher.data(), cipher.size(), plain.data(), plain.size(), &written);
    secureZero(key.data(), key.size());
    if (status != kCCSuccess) { secureZero(plain.data(), plain.size()); throw std::runtime_error("Cannot decrypt the secret"); }
    plain.resize(written);
    return plain;
}

// ---- Files, text and locations -------------------------------------------

void replaceFile(const std::filesystem::path& source, const std::filesystem::path& target) {
    ensure(::rename(source.c_str(), target.c_str()) == 0, "Cannot atomically replace file");
}

std::filesystem::path findExecutable(std::string_view name) {
#if TARGET_OS_OSX
    if (name.empty() || name.find('/') != std::string_view::npos || name.find('\0') != std::string_view::npos) return {};
    const char* path = std::getenv("PATH");
    std::string_view rest = path ? path : "/usr/bin:/bin:/usr/sbin:/sbin";
    while (true) {
        const auto colon = rest.find(':');
        const auto directory = rest.substr(0, colon);
        if (!directory.empty()) {
            const auto candidate = std::filesystem::path(std::string(directory)) / std::string(name);
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error) && ::access(candidate.c_str(), X_OK) == 0) return candidate;
        }
        if (colon == std::string_view::npos) break;
        rest.remove_prefix(colon + 1);
    }
#else
    (void)name;
#endif
    return {};
}

std::string decodeCodePage(unsigned codePage, std::string_view bytes) {
    if (bytes.empty()) return {};
    const CFStringEncoding encoding = CFStringConvertWindowsCodepageToEncoding(codePage);
    ensure(encoding != kCFStringEncodingInvalidId && CFStringIsEncodingAvailable(encoding), "This text encoding is unavailable");
    CFStringRef text = CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(bytes.data()),
        static_cast<CFIndex>(bytes.size()), encoding, false);
    ensure(text != nullptr, "File contains invalid bytes for its text encoding");
    NSString* string = (__bridge_transfer NSString*)text;
    return fromNSString(string);
}

std::string preferredLocale() {
    NSString* first = NSLocale.preferredLanguages.firstObject;
    return first ? fromNSString(first) : std::string("en-US");
}

unsigned long processId() { return static_cast<unsigned long>(::getpid()); }

std::filesystem::path executableDirectory() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) return {};
    path.resize(std::strlen(path.c_str()));
    std::error_code error;
    auto resolved = std::filesystem::weakly_canonical(path, error);
    return (error ? std::filesystem::path(path) : resolved).parent_path();
}

std::filesystem::path dataDirectory() {
    if (const char* override = std::getenv("VELIN_DATA_DIR")) {
        std::filesystem::path path(override);
        if (path.is_absolute()) return path;
    }
    NSURL* support = [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject;
    if (!support) return {};
    return std::filesystem::path(fromNSString(support.path)) / "Velin";
}

// ---- Helper processes -----------------------------------------------------

#if TARGET_OS_OSX
struct ChildProcess::Impl {
    pid_t pid = -1;
    int pipes[2] = {-1, -1};  // stdout, stderr read ends.
    bool open[2] = {false, false};
    bool exited = false;
    long long status = 0;
    void record(int value) {
        exited = true;
        status = WIFEXITED(value) ? WEXITSTATUS(value) : WIFSIGNALED(value) ? 128 + WTERMSIG(value) : 1;
    }
    ~Impl() { for (int fd : pipes) if (fd >= 0) ::close(fd); }
};

ChildProcess::ChildProcess(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
                           const std::filesystem::path& workingDirectory) : impl_(std::make_unique<Impl>()) {
    int output[2] = {-1, -1}, error[2] = {-1, -1};
    struct Close { int* fds; ~Close() { for (int i = 0; i < 2; ++i) if (fds[i] >= 0) ::close(fds[i]); } };
    ensure(::pipe(output) == 0, "Cannot create stdout pipe");
    Close closeOutput{output};
    ensure(::pipe(error) == 0, "Cannot create stderr pipe");
    Close closeError{error};
    std::vector<std::string> storage{executable.string()};
    for (const auto& argument : arguments) {
        ensure(argument.find('\0') == std::string::npos, "Process argument contains NUL");
        storage.push_back(argument);
    }
    std::vector<char*> argv;
    for (auto& value : storage) argv.push_back(value.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    ensure(posix_spawn_file_actions_init(&actions) == 0, "Cannot prepare helper process");
    struct Actions { posix_spawn_file_actions_t& value; ~Actions() { posix_spawn_file_actions_destroy(&value); } } actionsCleanup{actions};
    ensure(posix_spawnattr_init(&attributes) == 0, "Cannot prepare helper process");
    struct Attributes { posix_spawnattr_t& value; ~Attributes() { posix_spawnattr_destroy(&value); } } attributesCleanup{attributes};
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, error[1], STDERR_FILENO);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    // The non-_np spelling needs macOS 26; keep supporting older releases.
    ensure(posix_spawn_file_actions_addchdir_np(&actions, workingDirectory.c_str()) == 0, "Cannot set helper working directory");
#pragma clang diagnostic pop
    // Inherit only stdin/stdout/stderr, and run in a new process group so
    // terminate() reaches grandchildren too.
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF);
    posix_spawnattr_setpgroup(&attributes, 0);
    sigset_t defaults;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    posix_spawnattr_setsigdefault(&attributes, &defaults);
    const int result = posix_spawn(&impl_->pid, executable.c_str(), &actions, &attributes, argv.data(), environ);
    ensure(result == 0, std::string("Cannot start the helper process: ") + std::strerror(result));
    ::close(output[1]); output[1] = -1;
    ::close(error[1]); error[1] = -1;
    impl_->pipes[0] = output[0]; output[0] = -1;
    impl_->pipes[1] = error[0]; error[0] = -1;
    for (int i = 0; i < 2; ++i) {
        ::fcntl(impl_->pipes[i], F_SETFL, ::fcntl(impl_->pipes[i], F_GETFL) | O_NONBLOCK);
        ::fcntl(impl_->pipes[i], F_SETFD, FD_CLOEXEC);
        impl_->open[i] = true;
    }
}

ChildProcess::~ChildProcess() { if (impl_->pid > 0 && !impl_->exited) terminate(); }

std::size_t ChildProcess::read(bool standardError, char* buffer, std::size_t size) {
    const int index = standardError ? 1 : 0;
    if (!impl_->open[index]) return 0;
    const auto count = ::read(impl_->pipes[index], buffer, size);
    if (count > 0) return static_cast<std::size_t>(count);
    if (count == 0 || (errno != EAGAIN && errno != EINTR)) impl_->open[index] = false;  // EOF.
    return 0;
}

bool ChildProcess::wait(int milliseconds) {
    if (impl_->exited) return true;
    pollfd descriptors[2];
    nfds_t count = 0;
    for (int i = 0; i < 2; ++i) if (impl_->open[i]) descriptors[count++] = {impl_->pipes[i], POLLIN, 0};
    if (milliseconds > 0) {
        if (count) ::poll(descriptors, count, milliseconds);
        else ::usleep(static_cast<useconds_t>(milliseconds) * 1000);
    }
    int value = 0;
    const pid_t done = ::waitpid(impl_->pid, &value, WNOHANG);
    if (done == impl_->pid) impl_->record(value);
    ensure(done >= 0 || errno == EINTR, "Cannot obtain helper exit status");
    return impl_->exited;
}

void ChildProcess::terminate() {
    if (impl_->pid <= 0 || impl_->exited) return;
    ::kill(-impl_->pid, SIGKILL);
    ::kill(impl_->pid, SIGKILL);
    int value = 0;
    while (::waitpid(impl_->pid, &value, 0) < 0 && errno == EINTR) {}
    impl_->record(value);
}

long long ChildProcess::exitCode() const { return impl_->status; }

#else  // iOS has no helper processes.

struct ChildProcess::Impl {};
ChildProcess::ChildProcess(const std::filesystem::path&, const std::vector<std::string>&, const std::filesystem::path&) {
    throw std::runtime_error("Helper processes are not available on iOS");
}
ChildProcess::~ChildProcess() = default;
std::size_t ChildProcess::read(bool, char*, std::size_t) { return 0; }
bool ChildProcess::wait(int) { return true; }
void ChildProcess::terminate() {}
long long ChildProcess::exitCode() const { return 1; }

#endif

} // namespace velin::sys
