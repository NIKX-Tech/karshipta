/*
 *  IXWebSocketExport.h
 *  Added by NIKX-Tech/karshipta's gateway build (see gateway/patches/), not
 *  upstream ixwebsocket.
 *
 *  ixwebsocket has several classes with static const data members used as
 *  default-argument values in their own headers (WebSocketCloseConstants,
 *  SocketServer, WebSocketServer, WebSocket, WebSocketTransport, Socket,
 *  SelectInterrupt(Pipe)). None of them were annotated for export, which
 *  only matters - and only ever surfaces - when ixwebsocket is built as a
 *  genuine Windows DLL: MSVC does not auto-export data symbols from a DLL
 *  the way it does functions (unlike GCC/Clang shared libraries elsewhere),
 *  so any consumer relying on one of these defaults fails to link with an
 *  "unresolved external symbol" for the constant, not the function that
 *  uses it. IXWEBSOCKET_EXPORT on the affected classes fixes this the
 *  standard way, exporting every member (functions included, which already
 *  linked fine, and data, which didn't) as a single class-level annotation
 *  instead of hunting down and fixing one constant at a time as each one
 *  happens to get referenced by a new call site.
 *
 *  A no-op everywhere except an actual Windows shared build: elsewhere
 *  (macOS/Linux, or a static Windows build) this expands to nothing, the
 *  same as before this file existed.
 */
#pragma once

#if defined(_WIN32) && defined(IXWEBSOCKET_SHARED)
    #ifdef ixwebsocket_EXPORTS
        #define IXWEBSOCKET_EXPORT __declspec(dllexport)
    #else
        #define IXWEBSOCKET_EXPORT __declspec(dllimport)
    #endif
#else
    #define IXWEBSOCKET_EXPORT
#endif
