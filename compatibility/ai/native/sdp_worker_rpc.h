// Stack-bounded identity pins for dedicated native private-RPC sessions.
#ifndef SQL_SDP_WORKER_RPC_H
#define SQL_SDP_WORKER_RPC_H
#include <memory>
#include <string>
#include "sql/mysqld.h"
#include "sql/sdp_ai_catalog_guard.h"
#include "sql/sdp_ai_invoker.h"
#include "sql/sdp_ai_knowledge.h"
#include "sql/sdp_ai_commit_scope.h"

namespace sdp_native {
class Worker_rpc_guard final {
  std::unique_ptr<sdp_ai::Catalog_guard> m_ai;
  std::unique_ptr<sdp_ai_invoker::Worker_rpc_scope> m_scope;
  std::unique_ptr<sdp_ai::Commit_catalog_scope> m_commit_scope;
  // Database case behavior follows the server's case setting. Accent-insensitive
  // identifier comparison must not turn a different namespace into a trusted one.
  static bool schema_is(std::string name,const char *expected) {
    if (lower_case_table_names) for (char &c:name)
      if (c>='A' && c<='Z') c=static_cast<char>(c-'A'+'a');
    return name==expected;
  }
  static bool reject(THD *thd,bool identity=false,bool arguments=false) {
    return sdp_knowledge::error(thd,arguments?61006:61081,arguments?"0A000":"42000",
        arguments?"502006: Private AI RPC arguments require supported readonly expressions.":
          (identity?"510001: Private AI RPC catalog identity is not trusted.":"510001: Private AI RPC target is not permitted."));
  }
 public:
  bool pin(THD *thd,const char *schema,const char *procedure,
      const mem_root_deque<Item *> *arguments,bool native_producer,
      bool native_commit,bool execution) {
    if (native_producer) {
      m_ai=std::make_unique<sdp_ai::Catalog_guard>();
      if (m_ai->pin(thd)) return reject(thd,true);
      if (execution && native_commit)
        m_commit_scope=std::make_unique<sdp_ai::Commit_catalog_scope>(thd,m_ai.get());
      return false;
    }
    if (!thd->variables.sdp_ai_worker_rpc_guard) return false;
    const bool ai=schema && schema_is(schema,"sdp_ai");
    if (!ai) return false; // Ordinary user namespaces retain native ACLs.
    bool tools=false;
    const bool nested=sdp_ai_invoker::worker_rpc_active(thd);
    const bool known=sdp_ai::Catalog_guard::known_rpc(procedure,&tools);
    bool entry_tools=false;
    const bool entry=sdp_ai::Catalog_guard::worker_entry(procedure,&entry_tools);
    if (!known || (!nested && !entry)) return reject(thd);
    if (!nested && arguments) for (Item *argument:*arguments) {
      auto impure=[](Item *item) {
        return item->type()==Item::SUBQUERY_ITEM ||
            sdp_knowledge::pure_item(item,static_cast<size_t>(-1));
      };
      if (argument->walk(&Item::walk_helper_thunk<decltype(impure)>,
          enum_walk::PREFIX|enum_walk::SUBQUERY,reinterpret_cast<uchar *>(&impure)))
        return reject(thd,false,true);
    }
    m_ai=std::make_unique<sdp_ai::Catalog_guard>();
    if (m_ai->pin(thd,tools || entry_tools)) return reject(thd,true);
    // This is an origin marker, never a role or privilege grant. Each catalog
    // routine still checks the authenticated worker/reader and ordinary ACLs.
    if (execution) m_scope=std::make_unique<sdp_ai_invoker::Worker_rpc_scope>(thd);
    return false;
  }
};
}
#endif
