#include "mic_permission.h"

#import <AVFoundation/AVFoundation.h>

#include <atomic>

namespace rezonality
{

MicPermission query_mic_permission()
{
    switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio])
    {
    case AVAuthorizationStatusAuthorized:
        return MicPermission::Granted;
    case AVAuthorizationStatusDenied:
    case AVAuthorizationStatusRestricted:
        return MicPermission::Denied;
    case AVAuthorizationStatusNotDetermined:
    default:
    {
        static std::atomic<bool> requested{ false };
        if (!requested.exchange(true))
        {
            [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                                     completionHandler:^(BOOL){
                                     }];
        }
        return MicPermission::Pending;
    }
    }
}

} // namespace rezonality
