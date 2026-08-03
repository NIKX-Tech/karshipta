Overlay files copied on top of the fetched ixwebsocket v11.4.5 source (see
gateway/CMakeLists.txt's `FetchContent_Declare(ixwebsocket ...)`
`PATCH_COMMAND`), not a patch/diff file: `cmake -E copy_directory` just
replaces these filenames in the fetched tree wholesale, so each one here is
a real, complete, syntactically valid header, easy to read and diff against
upstream directly.

Every file here is the unmodified upstream header plus two changes:
`#include "IXWebSocketExport.h"` near the top, and `IXWEBSOCKET_EXPORT`
added to the one class/struct declaration each file defines.
`IXWebSocketExport.h` itself is new, not present upstream at all.

Why: on Windows, when ixwebsocket is built as a genuine shared library
(gateway/CMakeLists.txt forces this whenever relay is enabled, to avoid a
real double-init/double-free of `ix::initNetSystem`/`uninitNetSystem`
across relayly.dll and this executable - see relay-transport.md), MSVC does
not auto-export data symbols from the DLL the way it does functions. Every
class here has at least one `static const` member used as a default
argument value somewhere in ixwebsocket's own headers
(`WebSocketCloseConstants::kNormalClosureCode`,
`SocketServer::kDefaultTcpBacklog`, etc.) - any translation unit relying on
that default embeds a reference to the external data symbol, which never
resolves against an unexported Windows DLL, however many of ixwebsocket's
own *function* symbols link fine. Confirmed the hard way, across several
real Windows CI builds: symbols kept surfacing across different call sites
(first relayly's own code, then this gateway's own websocket_transport.cpp
and gt06_tcp_server.cpp) as each new one got exercised for the first time,
rather than one failure revealing the whole problem at once.

`IXWEBSOCKET_EXPORT` on the whole class (not per-member) fixes every
member at once - functions and data alike, present ones and any this
gateway doesn't happen to call yet - the standard Windows DLL export
pattern, not a one-symbol-at-a-time patch. It expands to nothing at all
outside an actual Windows shared build (see the header's own comment), so
this changes nothing on macOS/Linux or a static Windows build.

If ixwebsocket's own version pin (`GIT_TAG`/`URL` in
gateway/CMakeLists.txt) ever moves, diff these files against the new
version's originals before assuming they still apply cleanly - a
`copy_directory` overwrite has no way to warn about upstream changes to
the surrounding parts of a header it doesn't touch.
