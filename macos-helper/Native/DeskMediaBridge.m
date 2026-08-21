#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <CoreFoundation/CoreFoundation.h>
#import <dispatch/dispatch.h>

#include <stdio.h>

typedef void (*DeskMediaGetInfo)(dispatch_queue_t, void (^)(NSDictionary *));
typedef void (*DeskMediaGetPlaying)(dispatch_queue_t, void (^)(Boolean));
typedef void (*DeskMediaGetApplicationPID)(dispatch_queue_t, void (^)(int));
typedef void (*DeskMediaRegister)(dispatch_queue_t);
typedef Boolean (*DeskMediaSetEligible)(Boolean);

static void desk_media_write_json(NSDictionary *dictionary)
{
    NSError *error = nil;
    NSData *data = [NSJSONSerialization dataWithJSONObject:dictionary options:0 error:&error];
    if (data != nil && error == nil) {
        fwrite(data.bytes, 1, data.length, stdout);
    } else {
        fputs("{}", stdout);
    }
    fputc('\n', stdout);
    fflush(stdout);
}

__attribute__((visibility("default")))
void desk_media_snapshot(void)
{
    @autoreleasepool {
        NSURL *frameworkURL = [NSURL fileURLWithPath:
            @"/System/Library/PrivateFrameworks/MediaRemote.framework"];
        CFBundleRef framework = CFBundleCreate(kCFAllocatorDefault, (__bridge CFURLRef)frameworkURL);
        if (framework == NULL) {
            desk_media_write_json(@{});
            return;
        }

        DeskMediaGetInfo getInfo = (DeskMediaGetInfo)CFBundleGetFunctionPointerForName(
            framework, CFSTR("MRMediaRemoteGetNowPlayingInfo")
        );
        DeskMediaGetPlaying getPlaying = (DeskMediaGetPlaying)CFBundleGetFunctionPointerForName(
            framework, CFSTR("MRMediaRemoteGetNowPlayingApplicationIsPlaying")
        );
        DeskMediaGetApplicationPID getApplicationPID =
            (DeskMediaGetApplicationPID)CFBundleGetFunctionPointerForName(
                framework, CFSTR("MRMediaRemoteGetNowPlayingApplicationPID")
            );
        DeskMediaRegister registerNotifications = (DeskMediaRegister)CFBundleGetFunctionPointerForName(
            framework, CFSTR("MRMediaRemoteRegisterForNowPlayingNotifications")
        );
        DeskMediaSetEligible setEligible = (DeskMediaSetEligible)CFBundleGetFunctionPointerForName(
            framework, CFSTR("MRMediaRemoteSetCanBeNowPlayingApplication")
        );
        if (getInfo == NULL) {
            CFRelease(framework);
            desk_media_write_json(@{});
            return;
        }

        dispatch_queue_t queue = dispatch_queue_create(
            "com.dongyu.desk-console-helper.media-bridge",
            DISPATCH_QUEUE_SERIAL
        );
        if (registerNotifications != NULL) {
            registerNotifications(queue);
        }
        if (setEligible != NULL) {
            setEligible(false);
        }

        dispatch_group_t group = dispatch_group_create();
        __block NSDictionary *mediaInfo = nil;
        __block BOOL playing = NO;
        __block int applicationPID = -1;
        dispatch_group_enter(group);
        getInfo(queue, ^(NSDictionary *result) {
            mediaInfo = result;
            dispatch_group_leave(group);
        });
        if (getPlaying != NULL) {
            dispatch_group_enter(group);
            getPlaying(queue, ^(Boolean result) {
                playing = result ? YES : NO;
                dispatch_group_leave(group);
            });
        }
        if (getApplicationPID != NULL) {
            dispatch_group_enter(group);
            getApplicationPID(queue, ^(int result) {
                applicationPID = result;
                dispatch_group_leave(group);
            });
        }
        dispatch_group_wait(group, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));

        NSMutableDictionary *snapshot = [NSMutableDictionary dictionary];
        NSDictionary<NSString *, NSString *> *textKeys = @{
            @"title": @"kMRMediaRemoteNowPlayingInfoTitle",
            @"artist": @"kMRMediaRemoteNowPlayingInfoArtist",
            @"source": @"kMRMediaRemoteNowPlayingInfoClientBundleIdentifier",
        };
        for (NSString *outputKey in textKeys) {
            id value = mediaInfo[textKeys[outputKey]];
            if ([value isKindOfClass:NSString.class] && [value length] > 0) {
                snapshot[outputKey] = value;
            }
        }
        if (snapshot[@"source"] == nil && applicationPID > 0) {
            NSRunningApplication *application =
                [NSRunningApplication runningApplicationWithProcessIdentifier:applicationPID];
            if (application.bundleIdentifier.length > 0) {
                snapshot[@"source"] = application.bundleIdentifier;
            }
        }
        NSDictionary<NSString *, NSString *> *numberKeys = @{
            @"duration": @"kMRMediaRemoteNowPlayingInfoDuration",
            @"position": @"kMRMediaRemoteNowPlayingInfoElapsedTime",
        };
        for (NSString *outputKey in numberKeys) {
            id value = mediaInfo[numberKeys[outputKey]];
            if ([value isKindOfClass:NSNumber.class]) {
                snapshot[outputKey] = value;
            }
        }
        snapshot[@"playing"] = @(playing);
        desk_media_write_json(snapshot);
        CFRelease(framework);
    }
}
