#pragma once
#include "folio_weread_client.h"
namespace folio::weread {
enum class SessionLoad { Missing, Loaded, Invalid, Failed };
SessionLoad LoadSession(Session* out);
bool SaveSession(const Session& session);
bool EraseSession();
}
