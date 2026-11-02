// Gives the memory a closed chat freed back to the system. Main thread only.
#pragma once

namespace Contrail {

// glibc keeps freed blocks inside its heap: after a chat with pictures the process stays tens of MiB larger
// though nothing uses that memory. malloc_trim returns the whole free pages. It locks each malloc arena while it
// walks it (1–4 ms on the FLX1s), so it runs once things have settled: `delayMs` after the last call and as long
// after the last PostponeMemoryTrim. The default is long enough for GTK to drop a closed chat's widgets and
// textures, which goes on after the chat is gone.
void TrimMemorySoon(int delayMs = 2000);

// Something is going on (a chat is being scrolled): a trim due now waits until it is over. Cheap enough to call on
// every scroll step.
void PostponeMemoryTrim();

}  // namespace Contrail
