local STATUS_CODES = {200, 200, 200, 200, 301, 404, 500}
local PATHS = {"/", "/index.html", "/api/users", "/api/orders", "/static/app.js", "/favicon.ico"}

local function split(s, sep)
    local parts = {}
    for part in string.gmatch(s, "([^" .. sep .. "]+)") do
        table.insert(parts, part)
    end
    return parts
end

local function make_log_lines(n, status_codes, paths)
    local lines = {}
    local i = 0
    while i < n do
        local status = status_codes[(i % #status_codes) + 1]
        local path = paths[(i % #paths) + 1]
        local line = string.format("%d GET %s %d %d", i, path, status, i * 37 % 9973)
        table.insert(lines, line)
        i = i + 1
    end
    return lines
end

local function process_lines(lines)
    local status_counts = {}
    local path_counts = {}
    local total_bytes = 0
    for _, line in ipairs(lines) do
        local parts = split(line, " ")
        -- Lua tables are 1-indexed (unlike AER's 0-indexed arrays) -- part 0 ("{i}") is parts[1] here.
        local path = parts[3]
        local status = parts[4]
        local nbytes = tonumber(parts[5])

        status_counts[status] = (status_counts[status] or 0) + 1
        path_counts[path] = (path_counts[path] or 0) + 1

        total_bytes = total_bytes + nbytes
    end
    return status_counts, path_counts, total_bytes
end

local N = 200000

local start = os.clock()
local lines = make_log_lines(N, STATUS_CODES, PATHS)
local status_counts, path_counts, total_bytes = process_lines(lines)
local dur = os.clock() - start

print(string.format("log_processing: %d lines in %fs, total_bytes=%d", N, dur, total_bytes))
print(string.format("  status 200: %d, 404: %d, 500: %d", status_counts["200"], status_counts["404"], status_counts["500"]))
print(string.format("  path /: %d, /api/users: %d", path_counts["/"], path_counts["/api/users"]))
