export module pi.types.cache_scan_result;

import std;
export import pi.types.cache_previous_request;
export import pi.types.cache_waste_totals;

/** What a scan of a session branch leaves behind: the last request seen and the waste counted so far. */
export struct CacheScanResult {
    std::optional<CachePreviousRequest> previous;
    CacheWasteTotals totals;
};
