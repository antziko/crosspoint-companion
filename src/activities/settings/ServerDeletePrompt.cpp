#include "ServerDeletePrompt.h"

#include <I18n.h>
#include <Logging.h>

#include <string>

#include "KOReaderCredentialStore.h"
#include "OpdsServerStore.h"
#include "activities/util/ConfirmationActivity.h"

void promptDeleteOpdsServer(Activity& host, GfxRenderer& renderer, MappedInputManager& mappedInput, const int index,
                            const ServerDeletedFn done) {
  const auto* server = OPDS_STORE.getServer(static_cast<size_t>(index));
  if (!server) return;
  const std::string body = server->name.empty() ? server->url : server->name;
  host.startActivityForResultNoThrow<ConfirmationActivity>(
      [&host, index, done](const ActivityResult& res) {
        if (res.isCancelled) return;
        const bool removed = OPDS_STORE.removeServer(static_cast<size_t>(index));
        if (!removed) LOG_ERR("OPS", "Failed to remove OPDS server at index %d", index);
        done(host, index, removed);
      },
      renderer, mappedInput, tr(STR_DELETE_SERVER), body);
}

void promptDeleteKOReaderServer(Activity& host, GfxRenderer& renderer, MappedInputManager& mappedInput,
                                const int index, const ServerDeletedFn done) {
  const auto* server = KOREADER_STORE.getServer(static_cast<size_t>(index));
  if (!server) return;
  const std::string body = server->name.empty() ? server->serverUrl : server->name;
  host.startActivityForResultNoThrow<ConfirmationActivity>(
      [&host, index, done](const ActivityResult& res) {
        if (res.isCancelled) return;
        const bool removed = KOREADER_STORE.removeServer(static_cast<size_t>(index));
        if (!removed) LOG_ERR("KRS", "Failed to remove KOReader sync server at index %d", index);
        done(host, index, removed);
      },
      renderer, mappedInput, tr(STR_DELETE_SERVER), body);
}
