local M = {}

local namespace = vim.api.nvim_create_namespace("rezonality")
local flash_namespace = vim.api.nvim_create_namespace("rezonality_flash")
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
  registry_waiters = {},
  registry_error = nil,
  last_registry_refresh_ms = 0,
  instances = {},
  instances_by_source = {},
  files = {},
  flash_serial = 0,
  flash_tokens = {},
  flash_namespace = flash_namespace,
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
  apply_key = "<C-CR>",
  flash_ms = 750,
  draxul_command = nil,
  server_runtime_dir = nil,
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
  elseif (vim.uv or vim.loop).os_uname().sysname == "Darwin" then
    base = join(vim.env.HOME or "", "Library", "Caches")
  else
    base = vim.env.XDG_CACHE_HOME or join(vim.env.HOME or "", ".cache")
  end
  return join(base or "", "draxul", "cache", "plugins",
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

local file_kinds = {
  scenegraph = "scene",
  vert = "vertex",
  frag = "fragment",
  geom = "geometry",
  rgen = "raygen",
  rmiss = "miss",
  rchit = "hit",
  metal = "metal",
  glsl = "include",
  h = "include",
}

local function display_path(path, project_path)
  local prefix = project_path ~= "" and project_path .. "/" or ""
  if prefix ~= "" and path:sub(1, #prefix) == prefix then
    return path:sub(#prefix + 1)
  end
  return path
end

local function rebuild_files()
  local by_path = {}
  local function append(document, instance)
    local raw_files = document and document.value.active_source_files or nil
    if type(raw_files) ~= "table" or #raw_files == 0 then
      raw_files = document and document.value.candidate_source_files or nil
    end
    if type(raw_files) ~= "table" then
      return
    end
    for _, raw_path in ipairs(raw_files) do
      local path = type(raw_path) == "string" and normalize(raw_path) or ""
      if path ~= "" then
        local entry = by_path[path]
        if not entry then
          local extension = path:match("%.([^./]+)$") or ""
          entry = {
            path = path,
            display_path = display_path(path, document.project_path),
            kind = file_kinds[string.lower(extension)] or "source",
            active_generation = tonumber(document.value.active_generation) or 0,
            instances = {},
            instance_ids = {},
          }
          by_path[path] = entry
        end
        local instance_id = instance.pane_id or document.id
        if not entry.instance_ids[instance_id] then
          entry.instance_ids[instance_id] = true
          table.insert(entry.instances, instance)
        end
        entry.active_generation = math.max(entry.active_generation,
          tonumber(document.value.active_generation) or 0)
      end
    end
  end

  if state.registry_available then
    for source, instances in pairs(state.instances_by_source) do
      local document = state.documents[source]
      for _, instance in ipairs(instances) do
        append(document, instance)
      end
    end
  else
    for _, document in pairs(state.documents) do
      append(document, {
        pane_id = document.id,
        label = document.project_path ~= "" and document.project_path
          or document.id,
        status = instance_status(document),
      })
    end
  end

  local files = {}
  for _, entry in pairs(by_path) do
    table.sort(entry.instances, function(left, right)
      return left.label < right.label
    end)
    local labels = {}
    local status = "LIVE"
    for _, instance in ipairs(entry.instances) do
      table.insert(labels, instance.label)
      if instance.status == "FAILED" then
        status = "FAILED"
      elseif status ~= "FAILED" and instance.status ~= "LIVE" then
        status = instance.status
      end
    end
    entry.status = status
    entry.source_text = table.concat(labels, ", ")
    entry.instance_ids = nil
    table.insert(files, entry)
  end
  table.sort(files, function(left, right)
    if left.display_path ~= right.display_path then
      return left.display_path < right.display_path
    end
    return left.path < right.path
  end)
  state.files = files
end

local function source_entry(path)
  for _, entry in ipairs(state.files) do
    if entry.path == path then
      return entry
    end
  end
  return nil
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
  local previous_key = vim.b[buffer].rezonality_apply_key
  local configured_key = state.config.apply_key
  local current_path = normalize(vim.api.nvim_buf_get_name(buffer))
  local wanted_key = type(configured_key) == "string"
      and configured_key ~= "" and source_entry(current_path)
      and configured_key or nil
  if previous_key ~= wanted_key then
    if previous_key then
      pcall(vim.keymap.del, { "n", "i" }, previous_key,
        { buffer = buffer })
    end
    if wanted_key then
      vim.keymap.set({ "n", "i" }, wanted_key, function()
        M.apply()
      end, {
        buffer = buffer,
        silent = true,
        desc = "Apply current shader to Rezonality",
      })
    end
    vim.b[buffer].rezonality_apply_key = wanted_key
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
  rebuild_files()
  rebuild_entries()
  apply_all_buffers()
end

local function default_server_runtime_dir()
  local base
  if is_windows() then
    base = vim.env.APPDATA
  elseif (vim.uv or vim.loop).os_uname().sysname == "Darwin" then
    base = vim.env.HOME and join(vim.env.HOME,
      "Library", "Application Support") or nil
  else
    base = vim.env.XDG_CONFIG_HOME
      or (vim.env.HOME and join(vim.env.HOME, ".config") or nil)
  end
  return base and join(base, "draxul", "runtime", "server-v1") or nil
end

local function server_runtime_dir()
  if state.config and state.config.server_runtime_dir
      and state.config.server_runtime_dir ~= "" then
    return state.config.server_runtime_dir
  elseif vim.env.DRAXUL_SERVER_RUNTIME_DIR
      and vim.env.DRAXUL_SERVER_RUNTIME_DIR ~= "" then
    return vim.env.DRAXUL_SERVER_RUNTIME_DIR
  end
  return default_server_runtime_dir()
end

local function server_metadata_executable()
  local runtime = server_runtime_dir()
  if not runtime then
    return nil
  end
  local selected
  local selected_timestamp = -1
  for _, path in ipairs(vim.fn.glob(join(runtime, "*.control.json"),
      false, true)) do
    local metadata = decode(path)
    local executable = metadata and metadata.client_executable
    local timestamp = metadata and tonumber(metadata.published_unix_ms) or -1
    if metadata and metadata.state == "ready"
        and type(executable) == "string" and executable ~= ""
        and vim.fn.executable(executable) == 1
        and timestamp > selected_timestamp then
      selected = executable
      selected_timestamp = timestamp
    end
  end
  return selected
end

local function draxul_executable()
  if state.config.draxul_command and state.config.draxul_command ~= "" then
    return state.config.draxul_command
  elseif vim.env.DRAXUL_EXECUTABLE and vim.env.DRAXUL_EXECUTABLE ~= "" then
    return vim.env.DRAXUL_EXECUTABLE
  end
  local advertised = server_metadata_executable()
  if advertised then
    return advertised
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
  local runtime = server_runtime_dir()
  if runtime and runtime ~= "" then
    vim.list_extend(args, { "--server-runtime-dir",
      runtime })
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
  local waiters = state.registry_waiters
  state.registry_waiters = {}
  for _, waiter in ipairs(waiters) do
    waiter()
  end
end

function M.refresh_registry(force, callback)
  if callback then
    table.insert(state.registry_waiters, callback)
  end
  if state.registry_pending then
    return
  end
  if not force and state.config.registry_refresh_ms > 0
      and now_ms() - state.last_registry_refresh_ms
        < state.config.registry_refresh_ms then
    local waiters = state.registry_waiters
    state.registry_waiters = {}
    for _, waiter in ipairs(waiters) do
      waiter()
    end
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

local function execute_control(instance, verb, ui_route)
  local args = verb == "focus"
      and route_args("pane", "focus", instance.pane_id)
    or route_args("pane", "action", instance.pane_id,
      "--action", "rezonality_reload")
  if not args then
    vim.notify("Rezonality: draxul executable not found", vim.log.levels.ERROR)
    return false
  end
  if ui_route and ui_route.control_id then
    table.insert(args, #args, "--ui")
    table.insert(args, #args, ui_route.control_id)
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

local function choose_ui_route(routes, callback)
  if type(routes) ~= "table" or #routes == 0 then
    callback(nil)
  elseif #routes == 1 then
    callback(routes[1])
  else
    vim.ui.select(routes, {
      prompt = "Focus in Draxul UI",
      format_item = function(route)
        return route.control_id or route.client_id or "Draxul UI"
      end,
    }, function(route)
      if route then
        callback(route)
      end
    end)
  end
end

local function run_control(instance, verb)
  if state.config.control_runner then
    return state.config.control_runner(verb, instance)
  end
  if verb ~= "focus" or (vim.env.DRAXUL_CONTROL_ID
      and vim.env.DRAXUL_CONTROL_ID ~= "") then
    return execute_control(instance, verb)
  end

  local args = route_args("ui", "list")
  if not args then
    return execute_control(instance, verb)
  end
  local function discovered(result)
    if result.code ~= 0 then
      execute_control(instance, verb)
      return
    end
    local ok, routes = pcall(vim.json.decode, result.stdout or "")
    choose_ui_route(ok and routes or nil, function(route)
      execute_control(instance, verb, route)
    end)
  end
  if vim.system then
    vim.system(args, { text = true }, vim.schedule_wrap(discovered))
  else
    local output = vim.fn.system(args)
    discovered({ code = vim.v.shell_error, stdout = output })
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

local function instances_for_current_file()
  local entry = source_entry(normalize(vim.api.nvim_buf_get_name(0)))
  return entry and entry.instances or {}
end

local function contextual_instances()
  local instances = instances_at_cursor()
  if #instances > 0 then
    return instances
  end
  instances = instances_for_current_file()
  return #instances > 0 and instances or state.instances
end

function M.focus()
  M.refresh()
  choose_instance(contextual_instances(),
    "Focus Rezonality pane", M.focus_instance)
end

function M.reload()
  M.refresh()
  choose_instance(contextual_instances(),
    "Reload Rezonality pane", M.reload_instance)
end

local flash_colors = {
  "#6b3106",
  "#c65b0a",
  "#ff9a22",
  "#d86b10",
  "#8a4008",
}

local function flash_buffer(buffer, window)
  if not vim.api.nvim_buf_is_valid(buffer) then
    return
  end
  if not vim.api.nvim_win_is_valid(window)
      or vim.api.nvim_win_get_buf(window) ~= buffer then
    window = nil
    for _, candidate in ipairs(vim.fn.win_findbuf(buffer)) do
      if vim.api.nvim_win_is_valid(candidate) then
        window = candidate
        break
      end
    end
  end
  if not window then
    return
  end

  state.flash_serial = state.flash_serial + 1
  local token = state.flash_serial
  state.flash_tokens[buffer] = token
  local group = "RezonalityApplyFlash" .. token
  vim.api.nvim_set_hl(0, group, { bg = flash_colors[1] })
  vim.api.nvim_buf_clear_namespace(buffer, flash_namespace, 0, -1)
  local first, last
  vim.api.nvim_win_call(window, function()
    first = vim.fn.line("w0") - 1
    last = vim.fn.line("w$") - 1
  end)
  for row = first, last do
    vim.api.nvim_buf_add_highlight(buffer, flash_namespace,
      group, row, 0, -1)
  end

  local duration = math.max(tonumber(state.config.flash_ms) or 750, 100)
  for index = 2, #flash_colors do
    local delay = math.floor(duration * (index - 1) / #flash_colors)
    local color = flash_colors[index]
    vim.defer_fn(function()
      if state.flash_tokens[buffer] == token then
        vim.api.nvim_set_hl(0, group, { bg = color })
      end
    end, delay)
  end
  vim.defer_fn(function()
    if state.flash_tokens[buffer] == token
        and vim.api.nvim_buf_is_valid(buffer) then
      vim.api.nvim_buf_clear_namespace(buffer, flash_namespace, 0, -1)
      state.flash_tokens[buffer] = nil
    end
  end, duration)
end

local function save_buffer(buffer)
  if not vim.bo[buffer].modified then
    return true
  end
  if vim.bo[buffer].buftype ~= ""
      or vim.api.nvim_buf_get_name(buffer) == "" then
    vim.notify("Rezonality: current buffer cannot be saved",
      vim.log.levels.ERROR)
    return false
  end
  local ok, err = pcall(vim.api.nvim_buf_call, buffer, function()
    vim.cmd("silent write")
  end)
  if not ok then
    vim.notify("Rezonality: save failed: " .. tostring(err),
      vim.log.levels.ERROR)
  end
  return ok
end

function M.apply()
  M.refresh()
  local buffer = vim.api.nvim_get_current_buf()
  local window = vim.api.nvim_get_current_win()
  choose_instance(contextual_instances(),
    "Apply shader to Rezonality pane", function(instance)
      if save_buffer(buffer) then
        flash_buffer(buffer, window)
        M.reload_instance(instance)
      end
    end)
end

function M.instances()
  M.refresh()
  return state.instances
end

function M.show_instances()
  M.refresh()
  choose_instance(state.instances, "Rezonality instances", M.focus_instance)
end

function M.files()
  M.refresh()
  return state.files
end

local function open_file(entry)
  vim.cmd("edit " .. vim.fn.fnameescape(entry.path))
end

function M.show_files()
  M.refresh()
  M.refresh_registry(true, function()
    if #state.files == 0 then
      vim.notify("Rezonality: no active shader files", vim.log.levels.INFO)
    elseif #state.files == 1 then
      open_file(state.files[1])
    else
      vim.ui.select(state.files, {
        prompt = "Rezonality active shader files",
        format_item = function(entry)
          return string.format("%-8s %-8s %s — %s", entry.status,
            entry.kind, entry.display_path, entry.source_text)
        end,
      }, function(entry)
        if entry then
          open_file(entry)
        end
      end)
    end
  end)
end

function M.status()
  M.refresh()
  M.refresh_registry(true, function()
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
      or string.format("diagnostic-cache fallback: %s",
        state.registry_error or "registry unavailable")
    vim.api.nvim_echo({ { string.format(
      "Rezonality: %d diagnostics in %d files; %d active sources; %s (%s)",
      #state.entries, file_count, #state.files, registry,
      state.diagnostics_dir) } },
      true, {})
  end)
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
  local function command(short_name, long_name, callback, options)
    vim.api.nvim_create_user_command(short_name, callback, options)
    vim.api.nvim_create_user_command(long_name, callback, options)
  end
  command("RezRefresh", "RezonalityRefresh", function()
    M.refresh_registry(true)
    M.refresh()
  end, {})
  command("RezProblems", "RezonalityProblems", function(options)
    M.problems(not options.bang)
  end, { bang = true })
  command("RezInstances", "RezonalityInstances", M.show_instances, {})
  command("RezFiles", "RezonalityFiles", M.show_files, {})
  command("RezApply", "RezonalityApply", M.apply, {})
  command("RezFocus", "RezonalityFocus", M.focus, {})
  command("RezReload", "RezonalityReload", M.reload, {})
  command("RezStatus", "RezonalityStatus", M.status, {})
  command("RezEnable", "RezonalityEnable", M.enable, {})
  command("RezDisable", "RezonalityDisable", M.disable, {})
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

function M._draxul_executable()
  return draxul_executable()
end

return M
