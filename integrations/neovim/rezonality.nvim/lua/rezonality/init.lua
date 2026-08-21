local M = {}

local namespace = vim.api.nvim_create_namespace("rezonality")
local state = {
  config = nil,
  diagnostics_dir = nil,
  installed_after_ms = 0,
  documents = {},
  entries = {},
  by_path = {},
  registry_panes = {},
  registry_available = false,
  registry_pending = false,
  registry_error = nil,
  last_registry_refresh_ms = 0,
  instances = {},
  instances_by_source = {},
  timer = nil,
  enabled = false,
  commands_created = false,
}

local defaults = {
  refresh_ms = 500,
  registry_refresh_ms = 2000,
  auto_refresh = true,
  virtual_text = true,
  signs = true,
  underline = true,
  draxul_command = nil,
  registry_provider = nil,
  control_runner = nil,
}

local function join(...)
  return table.concat({ ... }, "/"):gsub("/+", "/")
end

local function is_windows()
  return (vim.uv or vim.loop).os_uname().sysname == "Windows_NT"
end

local function normalize(path)
  if not path or path == "" then
    return ""
  end
  local result = vim.fs and vim.fs.normalize
      and vim.fs.normalize(path) or vim.fn.fnamemodify(path, ":p")
  result = (vim.uv or vim.loop).fs_realpath(result) or result
  result = result:gsub("\\", "/"):gsub("/$", "")
  return is_windows() and result:lower() or result
end

local function default_diagnostics_dir()
  if vim.env.REZONALITY_DIAGNOSTICS_DIR
      and vim.env.REZONALITY_DIAGNOSTICS_DIR ~= "" then
    return vim.env.REZONALITY_DIAGNOSTICS_DIR
  end
  local base
  if is_windows() then
    base = vim.env.LOCALAPPDATA or vim.env.APPDATA
    return join(base or "", "draxul", "cache", "plugins",
      "dev.draxul.rezonality", "diagnostics")
  elseif (vim.uv or vim.loop).os_uname().sysname == "Darwin" then
    base = join(vim.env.HOME or "", "Library", "Caches")
  else
    base = vim.env.XDG_CACHE_HOME or join(vim.env.HOME or "", ".cache")
  end
  return join(base or "", "draxul", "plugins",
    "dev.draxul.rezonality", "diagnostics")
end

local function decode(path)
  local file = io.open(path, "rb")
  if not file then
    return nil
  end
  local text = file:read("*a")
  file:close()
  local ok, value = pcall(vim.json.decode, text)
  return ok and type(value) == "table" and value or nil
end

local function install_epoch()
  local source = debug.getinfo(1, "S").source
  if source:sub(1, 1) ~= "@" then
    return 0
  end
  local marker_path = join(vim.fn.fnamemodify(source:sub(2), ":h"),
    "installed_at")
  local marker = io.open(marker_path, "rb")
  if not marker then
    return 0
  end
  local value = tonumber(marker:read("*a")) or 0
  marker:close()
  return value
end

local severity = {
  error = vim.diagnostic.severity.ERROR,
  warning = vim.diagnostic.severity.WARN,
  warn = vim.diagnostic.severity.WARN,
  info = vim.diagnostic.severity.INFO,
  hint = vim.diagnostic.severity.HINT,
}

local function source_id(path)
  return vim.fn.fnamemodify(path, ":t:r")
end

local function now_ms()
  return math.floor((vim.uv or vim.loop).hrtime() / 1000000)
end

local function load_documents()
  local documents = {}
  for _, path in ipairs(vim.fn.glob(join(state.diagnostics_dir, "*.json"),
      false, true)) do
    local value = decode(path)
    local timestamp = value and tonumber(value.timestamp_unix_ms)
    if value and (state.installed_after_ms == 0
        or (timestamp and timestamp >= state.installed_after_ms)) then
      local id = source_id(path)
      documents[id] = {
        id = id,
        project_path = normalize(value.project_path),
        value = value,
      }
    end
  end
  state.documents = documents
end

local function parse_plugin_config(pane)
  local value = pane.client_plugin_config_json
  if type(value) == "table" then
    return value
  end
  if type(value) ~= "string" or value == "" then
    return {}
  end
  local ok, decoded = pcall(vim.json.decode, value)
  return ok and type(decoded) == "table" and decoded or {}
end

local function document_has_error(document)
  local raw = document.value
  if string.lower(raw.severity or "") == "error" then
    return true
  end
  for _, entry in ipairs(type(raw.diagnostics) == "table"
      and raw.diagnostics or {}) do
    if type(entry) == "table"
        and string.lower(entry.severity or "error") == "error" then
      return true
    end
  end
  return false
end

local function instance_status(document)
  if not document then
    return "PENDING"
  end
  if document_has_error(document) then
    return "FAILED"
  end
  local raw = document.value
  local attempted = tonumber(raw.attempted_generation) or 0
  local active = tonumber(raw.active_generation) or 0
  if active > 0 and active == attempted then
    return "LIVE"
  elseif active > 0 then
    return "READY"
  elseif attempted > 0 then
    return "BUILDING"
  end
  return "PENDING"
end

local function rebuild_instances()
  local instances = {}
  local by_source = {}
  if state.registry_available then
    for _, pane in ipairs(state.registry_panes) do
      if pane.client_plugin_id == "dev.draxul.rezonality" then
        local config = parse_plugin_config(pane)
        local source = type(config.diagnostics_id) == "string"
          and config.diagnostics_id or ""
        local project_path = normalize(config.project_path)
        if source == "" and project_path ~= "" then
          for id, document in pairs(state.documents) do
            if document.project_path == project_path then
              source = id
              break
            end
          end
        end
        local document = source ~= "" and state.documents[source] or nil
        local space_name = pane.space_name or pane.space_id or "Space"
        local tab_name = pane.tab_name or pane.tab_id or "Tab"
        local pane_name = pane.name or pane.id or "Pane"
        local instance = {
          pane_id = pane.id,
          space_id = pane.space_id,
          tab_id = pane.tab_id,
          space_name = space_name,
          tab_name = tab_name,
          pane_name = pane_name,
          label = string.format("%s / %s / %s",
            space_name, tab_name, pane_name),
          source_id = source,
          project_path = project_path,
          document = document,
          status = instance_status(document),
        }
        table.insert(instances, instance)
        if source ~= "" then
          by_source[source] = by_source[source] or {}
          table.insert(by_source[source], instance)
        end
      end
    end
  end
  table.sort(instances, function(left, right)
    return left.label < right.label
  end)
  state.instances = instances
  state.instances_by_source = by_source
end

local function source_label(id)
  local instances = state.instances_by_source[id]
  if not instances or #instances == 0 then
    return id
  end
  local labels = {}
  for _, instance in ipairs(instances) do
    table.insert(labels, instance.label)
  end
  table.sort(labels)
  return table.concat(labels, ", ")
end

local function append_document(found, document)
  local raw_document = document.value
  local raw_entries = raw_document.diagnostics
  if type(raw_entries) ~= "table" then
    raw_entries = { raw_document }
  end
  for _, raw in ipairs(raw_entries) do
    if type(raw) == "table" and type(raw.message) == "string"
        and raw.message ~= "" and type(raw.path) == "string"
        and raw.path ~= "" then
      local path = raw.path
      if not path:match("^%a:[/\\]") and path:sub(1, 1) ~= "/"
          and type(raw_document.project_path) == "string" then
        path = join(raw_document.project_path, path)
      end
      local normalized = normalize(path)
      if normalized ~= "" then
        table.insert(found, {
          path = normalized,
          line = math.max(tonumber(raw.line) or 1, 1),
          column = math.max(tonumber(raw.column) or 1, 1),
          severity = severity[string.lower(raw.severity or "error")]
            or vim.diagnostic.severity.ERROR,
          message = raw.message,
          stage = raw.stage or raw_document.stage or "compile",
          source_id = document.id,
          attempted_generation = raw_document.attempted_generation or 0,
        })
      end
    end
  end
end

local function rebuild_entries()
  local found = {}
  local active_sources = {}
  if state.registry_available then
    for source in pairs(state.instances_by_source) do
      active_sources[source] = true
    end
  end
  for id, document in pairs(state.documents) do
    if not state.registry_available or active_sources[id] then
      append_document(found, document)
    end
  end

  local unique = {}
  local ordered = {}
  for _, entry in ipairs(found) do
    local key = table.concat({ entry.path, tostring(entry.line),
      tostring(entry.column), tostring(entry.severity), entry.message }, "\0")
    local existing = unique[key]
    if existing then
      existing.sources[entry.source_id] = true
    else
      entry.sources = { [entry.source_id] = true }
      unique[key] = entry
      table.insert(ordered, entry)
    end
  end
  table.sort(ordered, function(left, right)
    if left.path ~= right.path then
      return left.path < right.path
    elseif left.line ~= right.line then
      return left.line < right.line
    elseif left.column ~= right.column then
      return left.column < right.column
    end
    return left.message < right.message
  end)

  local by_path = {}
  for _, entry in ipairs(ordered) do
    local sources = {}
    for id in pairs(entry.sources) do
      table.insert(sources, source_label(id))
    end
    table.sort(sources)
    entry.source_text = "Rezonality (" .. table.concat(sources, ", ") .. ")"
    by_path[entry.path] = by_path[entry.path] or {}
    table.insert(by_path[entry.path], entry)
  end
  state.entries = ordered
  state.by_path = by_path
end

local function apply_buffer(buffer)
  if not vim.api.nvim_buf_is_valid(buffer) then
    return
  end
  vim.diagnostic.reset(namespace, buffer)
  local entries = state.by_path[normalize(vim.api.nvim_buf_get_name(buffer))]
  if not entries then
    return
  end
  local diagnostics = {}
  for _, entry in ipairs(entries) do
    table.insert(diagnostics, {
      lnum = entry.line - 1,
      col = entry.column - 1,
      severity = entry.severity,
      message = entry.message,
      source = entry.source_text,
      user_data = {
        stage = entry.stage,
        sources = entry.sources,
        attempted_generation = entry.attempted_generation,
      },
    })
  end
  vim.diagnostic.set(namespace, buffer, diagnostics, {
    virtual_text = state.config.virtual_text,
    signs = state.config.signs,
    underline = state.config.underline,
    severity_sort = true,
    update_in_insert = false,
  })
end

local function apply_all_buffers()
  for _, buffer in ipairs(vim.api.nvim_list_bufs()) do
    if vim.api.nvim_buf_is_loaded(buffer) then
      apply_buffer(buffer)
    end
  end
end

local function rebuild()
  rebuild_instances()
  rebuild_entries()
  apply_all_buffers()
end

local function draxul_executable()
  if state.config.draxul_command and state.config.draxul_command ~= "" then
    return state.config.draxul_command
  elseif vim.env.DRAXUL_EXECUTABLE and vim.env.DRAXUL_EXECUTABLE ~= "" then
    return vim.env.DRAXUL_EXECUTABLE
  end
  local discovered = vim.fn.exepath("draxul")
  return discovered ~= "" and discovered or nil
end

local function route_args(...)
  local executable = draxul_executable()
  if not executable then
    return nil
  end
  local args = { executable, ... }
  if vim.env.DRAXUL_SESSION_ID and vim.env.DRAXUL_SESSION_ID ~= "" then
    vim.list_extend(args, { "--session", vim.env.DRAXUL_SESSION_ID })
  end
  if vim.env.DRAXUL_SERVER_RUNTIME_DIR
      and vim.env.DRAXUL_SERVER_RUNTIME_DIR ~= "" then
    vim.list_extend(args, { "--server-runtime-dir",
      vim.env.DRAXUL_SERVER_RUNTIME_DIR })
  end
  table.insert(args, "--json")
  return args
end

local function finish_registry(panes, err)
  state.registry_pending = false
  state.last_registry_refresh_ms = now_ms()
  if type(panes) == "table" then
    state.registry_panes = panes
    state.registry_available = true
    state.registry_error = nil
  else
    state.registry_panes = {}
    state.registry_available = false
    state.registry_error = err or "Draxul pane registry is unavailable"
  end
  rebuild()
end

function M.refresh_registry(force)
  if state.registry_pending then
    return
  end
  if not force and state.config.registry_refresh_ms > 0
      and now_ms() - state.last_registry_refresh_ms
        < state.config.registry_refresh_ms then
    return
  end
  state.registry_pending = true
  if state.config.registry_provider then
    local ok, panes, err = pcall(state.config.registry_provider)
    finish_registry(ok and panes or nil, ok and err or panes)
    return
  end

  local args = route_args("pane", "list")
  if not args then
    finish_registry(nil, "draxul executable not found")
    return
  end
  if vim.system then
    vim.system(args, { text = true }, vim.schedule_wrap(function(result)
      if result.code ~= 0 then
        finish_registry(nil, result.stderr ~= "" and result.stderr
          or "pane registry command failed")
        return
      end
      local ok, panes = pcall(vim.json.decode, result.stdout)
      finish_registry(ok and panes or nil,
        ok and nil or "pane registry returned invalid JSON")
    end))
    return
  end
  local output = vim.fn.system(args)
  if vim.v.shell_error ~= 0 then
    finish_registry(nil, output)
    return
  end
  local ok, panes = pcall(vim.json.decode, output)
  finish_registry(ok and panes or nil,
    ok and nil or "pane registry returned invalid JSON")
end

function M.refresh()
  if not state.enabled then
    return
  end
  load_documents()
  rebuild()
  M.refresh_registry(false)
end

local function quickfix_type(value)
  if value == vim.diagnostic.severity.ERROR then
    return "E"
  elseif value == vim.diagnostic.severity.WARN then
    return "W"
  elseif value == vim.diagnostic.severity.INFO then
    return "I"
  end
  return "N"
end

function M.problems(open_window)
  M.refresh()
  local items = {}
  for _, entry in ipairs(state.entries) do
    table.insert(items, {
      filename = entry.path,
      lnum = entry.line,
      col = entry.column,
      text = entry.message .. " [" .. entry.source_text .. "]",
      type = quickfix_type(entry.severity),
    })
  end
  vim.fn.setqflist({}, "r", { title = "Rezonality diagnostics", items = items })
  if open_window ~= false and #items > 0 then
    vim.cmd("copen")
  elseif #items == 0 then
    vim.notify("Rezonality: no diagnostics", vim.log.levels.INFO)
  end
  return items
end

local function run_control(instance, verb)
  if state.config.control_runner then
    return state.config.control_runner(verb, instance)
  end
  local args = verb == "focus"
      and route_args("pane", "focus", instance.pane_id)
    or route_args("pane", "action", instance.pane_id,
      "--action", "rezonality_reload")
  if not args then
    vim.notify("Rezonality: draxul executable not found", vim.log.levels.ERROR)
    return false
  end
  local function completed(result)
    if result.code ~= 0 then
      vim.notify("Rezonality: " .. verb .. " failed: "
        .. (result.stderr or ""), vim.log.levels.ERROR)
    end
  end
  if vim.system then
    vim.system(args, { text = true }, vim.schedule_wrap(completed))
  else
    local output = vim.fn.system(args)
    completed({ code = vim.v.shell_error, stderr = output })
  end
  return true
end

function M.focus_instance(instance)
  return run_control(instance, "focus")
end

function M.reload_instance(instance)
  return run_control(instance, "reload")
end

local function choose_instance(instances, prompt, callback)
  if #instances == 0 then
    vim.notify("Rezonality: no matching live pane", vim.log.levels.WARN)
  elseif #instances == 1 then
    callback(instances[1])
  else
    vim.ui.select(instances, {
      prompt = prompt,
      format_item = function(instance)
        return string.format("%-8s %s", instance.status, instance.label)
      end,
    }, function(instance)
      if instance then
        callback(instance)
      end
    end)
  end
end

local function instances_at_cursor()
  local path = normalize(vim.api.nvim_buf_get_name(0))
  local line = vim.api.nvim_win_get_cursor(0)[1]
  local selected, seen = {}, {}
  for _, entry in ipairs(state.by_path[path] or {}) do
    if entry.line == line then
      for source in pairs(entry.sources) do
        for _, instance in ipairs(state.instances_by_source[source] or {}) do
          if not seen[instance.pane_id] then
            seen[instance.pane_id] = true
            table.insert(selected, instance)
          end
        end
      end
    end
  end
  return selected
end

function M.focus()
  M.refresh()
  local instances = instances_at_cursor()
  choose_instance(#instances > 0 and instances or state.instances,
    "Focus Rezonality pane", M.focus_instance)
end

function M.reload()
  M.refresh()
  local instances = instances_at_cursor()
  choose_instance(#instances > 0 and instances or state.instances,
    "Reload Rezonality pane", M.reload_instance)
end

function M.instances()
  M.refresh()
  return state.instances
end

function M.show_instances()
  M.refresh()
  choose_instance(state.instances, "Rezonality instances", M.focus_instance)
end

function M.status()
  M.refresh()
  local files = {}
  for _, entry in ipairs(state.entries) do
    files[entry.path] = true
  end
  local file_count = 0
  for _ in pairs(files) do
    file_count = file_count + 1
  end
  local registry = state.registry_available
      and string.format("%d live panes", #state.instances)
    or "diagnostic-cache fallback"
  vim.notify(string.format(
    "Rezonality: %d diagnostics in %d files; %s (%s)",
    #state.entries, file_count, registry, state.diagnostics_dir),
    vim.log.levels.INFO)
end

local function stop_timer(close)
  if not state.timer then
    return
  end
  state.timer:stop()
  if close and not state.timer:is_closing() then
    state.timer:close()
    state.timer = nil
  end
end

function M.disable()
  state.enabled = false
  stop_timer(false)
  for _, buffer in ipairs(vim.api.nvim_list_bufs()) do
    vim.diagnostic.reset(namespace, buffer)
  end
end

function M.enable()
  state.enabled = true
  M.refresh()
  if state.config.auto_refresh and state.config.refresh_ms > 0 then
    local uv = vim.uv or vim.loop
    state.timer = state.timer or uv.new_timer()
    state.timer:start(state.config.refresh_ms, state.config.refresh_ms,
      vim.schedule_wrap(M.refresh))
  end
end

local function create_commands()
  if state.commands_created then
    return
  end
  state.commands_created = true
  vim.api.nvim_create_user_command("RezonalityRefresh", function()
    M.refresh_registry(true)
    M.refresh()
  end, {})
  vim.api.nvim_create_user_command("RezonalityProblems", function(options)
    M.problems(not options.bang)
  end, { bang = true })
  vim.api.nvim_create_user_command("RezonalityInstances", M.show_instances, {})
  vim.api.nvim_create_user_command("RezonalityFocus", M.focus, {})
  vim.api.nvim_create_user_command("RezonalityReload", M.reload, {})
  vim.api.nvim_create_user_command("RezonalityStatus", M.status, {})
  vim.api.nvim_create_user_command("RezonalityEnable", M.enable, {})
  vim.api.nvim_create_user_command("RezonalityDisable", M.disable, {})
end

function M.setup(options)
  state.config = vim.tbl_deep_extend("force", defaults, options or {})
  state.diagnostics_dir = state.config.diagnostics_dir or default_diagnostics_dir()
  state.installed_after_ms = install_epoch()
  state.last_registry_refresh_ms = 0
  create_commands()
  local group = vim.api.nvim_create_augroup("RezonalityDiagnostics", { clear = true })
  vim.api.nvim_create_autocmd({ "BufReadPost", "BufEnter" }, {
    group = group,
    callback = function(args)
      if state.enabled then
        apply_buffer(args.buf)
      end
    end,
  })
  vim.api.nvim_create_autocmd("VimLeavePre", {
    group = group,
    callback = function()
      stop_timer(true)
    end,
  })
  M.enable()
end

function M._state()
  return state
end

return M
