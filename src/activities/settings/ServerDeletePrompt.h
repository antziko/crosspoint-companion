#pragma once

class Activity;
class GfxRenderer;
class MappedInputManager;

// The Delete Server confirmation shared by each store's editor and list. After a confirmed
// delete, done(host, index, removed) runs on the host; removed is false when the store refused.
using ServerDeletedFn = void (*)(Activity& host, int index, bool removed);

void promptDeleteOpdsServer(Activity& host, GfxRenderer& renderer, MappedInputManager& mappedInput, int index,
                            ServerDeletedFn done);
void promptDeleteKOReaderServer(Activity& host, GfxRenderer& renderer, MappedInputManager& mappedInput, int index,
                                ServerDeletedFn done);
