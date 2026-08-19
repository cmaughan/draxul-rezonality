#pragma once

namespace rezonality
{

enum class MicPermission
{
    Pending,
    Granted,
    Denied,
};

MicPermission query_mic_permission();

} // namespace rezonality
