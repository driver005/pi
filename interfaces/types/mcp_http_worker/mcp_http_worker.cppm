export module pi.types.mcp_http_worker;

import std;

/** A background thread of the HTTP transport; `finished` flips when the thread body is done. */
export struct McpHttpWorker {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> finished;
};
