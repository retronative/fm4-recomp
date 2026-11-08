# Offline sockets, option B: what FM4's callers need

Design (option B): runtime-only UDP sockets; creation, configuration and binding succeed locally; the link stays
offline; no host socket. Traced in our translation (ppc/; function traces not distributed).
Wrapper layer: one thin wrapper per NetDll import inserts caller 1 (r3) and shifts the arguments; args 9-10 of
WSARecvFrom/WSASendTo (overlapped, completion routine) are the stack slots at r1+0x54/+0x5C (runtime::Argument 8/9).

## Sections (cited from runtime/kernel/imports_net.cpp and the test)

- **probe** (function 82CE4828; reached indirectly, the call that option A failed): `socket(AF_INET, SOCK_DGRAM, 0)`;
  -1 -> E_NOTIMPL (0x80004001) and the child init fails (the crash seen under option A); otherwise `closesocket` at once.
- **session socket** (traced on the main menu): `socket(2, 2, 17)` (UDP) or `(2, 2, 254)` (VDP) by a
  flag at +304; UDP: `setsockopt(SOL_SOCKET, SO_BROADCAST, 1)` (failure -> E_FAIL); optional SO_RCVBUF (close/buffer
  sections); `setsockopt(SO_SNDBUF, 0)` (result only logged); bind; getsockname.
- **buffer query** (function 82CE11F8, indirect): presets the size 8192, then `socket(2,2,0)`,
  `getsockopt(SOL_SOCKET, SO_RCVBUF)` -> the size if it succeeds, `closesocket`.
- **bind** (function 82CDFB00): a fixed port binds once (failure -> 0x80158080); otherwise ports are tried in a range
  (2302-2400 or 1000-1255), 10013 (WSAEACCES) and 10048 (WSAEADDRINUSE) move to the next port, other errors fail.
- **getsockname** (function 82CE22A0): failure drops the session address (the session set-up fails).
- **receive** (function 82CE3C18): overlapped `WSARecvFrom` (overlapped from FM4's I/O manager; completion routine 0).
  0 -> `WSAGetOverlappedResult`; -1: 997 (WSA_IO_PENDING) -> the manager waits on the event (vtable +32);
  10038 -> E_FAIL; 10054 (WSAECONNRESET) -> re-issued at once (a spin if repeated); anything else (10035 included)
  -> E_FAIL and the session is released. => offline: pending forever, never 10035/10054.
- **completion** (function 82CE25B0, the I/O manager's callback): if the session socket is -1 (closed) the error is
  10038 without a call; else `WSAGetOverlappedResult(..., wait 0)`; then re-issues the receive while the session
  state is 2.
- **send** (function 82CE2240): `WSASendTo` plain (overlapped 0, completion 0); result ignored.
- **close** (function 82CDFA70): socket field set to -1, `shutdown(s, 0)`, `closesocket(s)`; errors only logged.

Second callers: functions 82CFD9F0 and 82CFD7F8 are interface shims (no direct callers) forwarding to the
same wrappers; their completion-routine slot is the shim's own uninitialised stack word, so a non-zero completion
routine stops rather than being trusted. INFERRED unreached on the frontend path (indirect only).

## Left as strict stops (decided from the trace)

- TCP (`socket(2, 1, 0)` + ioctlsocket/bind/listen/accept/recv/send, function 82B8DD70): reached only through
  indirect calls (82B46E68 <- 82B47878 <- 82B33208 <- 82B27358 <- 82B010F8, no direct caller); option B is scoped to UDP.
- XNet address imports (XNetGetTitleXnAddr, XNetXnAddrToInAddr, XNetConnect, inet_addr) behind the session request's
  address object: called only when the request carries one (82CE4F70 etc. pass an interface pointer from
  the request; the local path at 82CE5910 passes 0). UNRESOLVED statically whether the frontend sends one.
- recvfrom/sendto/select/ioctlsocket/WSAEventSelect: no FM4 caller on the UDP path.

## Deviation from the option B wording (recorded)

The option text said "receives return WSAEWOULDBLOCK". FM4's receive caller treats 10035 as a hard failure, so the
offline receive is an overlapped receive that stays pending (WSA_IO_PENDING; WSAGetOverlappedResult: 996), which is
also what an unplugged console does. closesocket aborts it (STATUS_CANCELLED, event set; 995), WinSock's behaviour.
Defaults SO_SNDBUF/SO_RCVBUF 8192: UNVERIFIED for the Xbox 360 (WinSock's documented default; FM4's own preset).
