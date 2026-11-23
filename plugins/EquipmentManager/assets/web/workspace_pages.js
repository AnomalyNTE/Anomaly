import { createLibraryPanel } from "./library.js";
import { createRolesPanel } from "./roles.js";
import { createAssessmentPanel } from "./assessment.js";
import { createAccountsPanel } from "./accounts.js";
import { createNavigation } from "./navigation.js";
import { createMultiOptimizer } from "./multi_optimizer.js";
import { createBlueprintPanel } from "./blueprints.js";
import { createRewindPanel } from "./rewind.js";
import { createSettingsPanel } from "./settings.js";
import { createCatalogPanel } from "./catalog.js";
import { createRolePanel } from "./role_panel.js";
import { createCultivationPanel } from "./cultivation.js";
import { createWarehouseRules } from "./warehouse_rules.js";
import { createQuickTidy } from "./quick_tidy.js";

export function createWorkspacePages({ api, t, node, workspace, inventory, features, settings,
  persist, isReady, currentRole, getDraft, loadDraft, selectedItem, reloadWorkspace,
  openCalculation, selectedAccountChanged, jobController, canRun, selectedUids, loadPlans, applyPlans, settingsChanged, refreshInventory, syncInventory, openOptimization, cancelOperations, inspectItem, trackOperation }) {
  const $ = (id) => document.getElementById(id);
  const read = (key) => settings().filters[key];
  const write = (key, value) => { settings().filters[key] = value; persist(); };
  let navigation;
  const library = createLibraryPanel({
    mount: $("library-root"), api, t, node, workspace, inventory, isReady, currentRole,
    readRole: () => read("library_role"), saveRole: (id) => write("library_role", id),
    getDraft, loadDraft, reloadWorkspace,
  });
  const roles = createRolesPanel({
    mount: $("roles-root"), api, t, node, workspace, features,
    readRole: () => read("role_profile"), saveRole: (id) => write("role_profile", id),
    calculationPreference: () => read("optimizer_preferences"), reloadWorkspace,
    openLibrary: (id) => { library.selectRole(id); navigation.go("library"); },
    openPanel: (id) => { panel.selectRole(id); navigation.go("panel"); },
    openCalculation,
  });
  const assessment = createAssessmentPanel({
    mount: $("assessment-root"), api, t, node, inventory, workspace, selectedItem, isReady,
    goEquipment: () => navigation.go("equipment"),
    readRoles: () => read("assessment_roles"), saveRoles: (value) => write("assessment_roles", value),
  });
  const accounts = createAccountsPanel({
    mount: $("accounts-root"), api, t, node, workspace, features, reloadWorkspace, selectedAccountChanged,
  });
  const saveSoftwarePlan = (plan) => { library.savePreview(plan); navigation.go("library"); };
  const multi = createMultiOptimizer({ mount: $("multi-root"), api, t, node, workspace, inventory, jobController, canRun, selectedUids, loadDraft, loadPlans, applyPlans, saveSoftwarePlan, reloadWorkspace,
    readPreference: () => read("multi_optimizer_preferences"), savePreference: (value) => write("multi_optimizer_preferences", value),
    readRolePreferences: () => read("optimizer_roles"), openPreference: openCalculation,
    readSaveSlots: () => read("batch_save_slots"), saveSaveSlots: (value) => write("batch_save_slots", value) });
  const blueprints = createBlueprintPanel({ mount: $("blueprint-root"), t, node, inventory, workspace, jobController,
    readPreference: () => read("blueprint_preferences"), savePreference: (value) => write("blueprint_preferences", value) });
  const rewind = createRewindPanel({ mount: $("rewind-root"), api, t, node, workspace, inventory, isReady,
    readPreference: () => read("rewind_preferences"), savePreference: (value) => write("rewind_preferences", value) });
  const preferences = createSettingsPanel({ mount: $("settings-root"), t, node, settings, persist, changed: settingsChanged, features,
    actions: [
      { id: "refresh", label: "web.shortcuts_refresh", run: syncInventory },
      { id: "search", label: "web.shortcuts_search", run: () => { navigation.go("equipment"); $("search").focus(); } },
      { id: "equipment", label: "web.nav_equipment", run: () => navigation.go("equipment") },
      { id: "library", label: "web.nav_library", feature: "library", run: () => navigation.go("library") },
      { id: "roles", label: "web.nav_roles", feature: "roles", run: () => navigation.go("roles") },
      { id: "optimize", label: "web.optimize_title", run: () => { navigation.go("equipment"); openOptimization(); } },
      { id: "multi", label: "web.nav_multi", feature: "optimize_multi", run: () => navigation.go("multi") },
    ] });
  const catalog = createCatalogPanel({ mount: $("catalog-root"), api, t, node,
    readPreference: () => read("catalog_preferences"), savePreference: (value) => write("catalog_preferences", value) });
  const panel = createRolePanel({ mount: $("role-panel-root"), api, t, node, workspace, inventory, features, jobController, reloadWorkspace,
    inspectItem: (uid, roleId) => { inspectItem(uid, roleId); navigation.go("equipment"); },
    readPreference: () => read("panel_preferences"), savePreference: (value) => write("panel_preferences", value) });
  const cultivation = createCultivationPanel({ mount: $("cultivation-root"), t, node, workspace, jobController, isReady,
    readPreference: () => read("cultivation_preferences"), savePreference: (value) => write("cultivation_preferences", value) });
  const rules = createWarehouseRules({ mount: $("warehouse-rules-root"), api, t, node, inventory, workspace, jobController, canApply: canRun, refreshInventory, cancelOperations,
    openActivity: () => { navigation.go("equipment"); $("activity").scrollIntoView({ block: "start", behavior: "smooth" }); },
    readPreference: () => read("warehouse_rules"), savePreference: (value) => write("warehouse_rules", value) });
  const tidy = createQuickTidy({ api, t, node, workspace, inventory, currentRole, jobController, canRun, reloadWorkspace, refreshInventory, trackOperation, cancelOperations,
    readPreference: () => read("quick_tidy"), savePreference: (value) => write("quick_tidy", value),
    openAdvanced: () => navigation.go("warehouse_rules") });
  for (const description of document.querySelectorAll(".page-heading p.meta")) description.remove();
  navigation = createNavigation({
    mount: $("primary-navigation"), t, node, features,
    readPage: () => read("primary_page"), savePage: (id) => write("primary_page", id),
    ready: () => Boolean(workspace()?.account),
    entries: [
      { id: "equipment", label: "web.nav_equipment", element: $("equipment-page") },
      { id: "multi", label: "web.nav_multi", feature: "optimize_multi", element: $("multi-page"), controller: multi },
      { id: "library", label: "web.nav_library", group: "tools", toolGroup: "manage", feature: "library", element: $("library-page"), controller: library },
      { id: "roles", label: "web.nav_roles", group: "tools", toolGroup: "manage", feature: "roles", element: $("roles-page"), controller: roles },
      { id: "accounts", label: "web.nav_accounts", group: "tools", toolGroup: "manage", feature: "accounts", element: $("accounts-page"), controller: accounts },
      { id: "panel", label: "web.nav_panel", group: "tools", toolGroup: "calculate", feature: "role_panel", element: $("role-panel-page"), controller: panel },
      { id: "cultivation", label: "web.nav_cultivation", group: "tools", toolGroup: "calculate", feature: "cultivation", element: $("cultivation-page"), controller: cultivation },
      { id: "assessment", label: "web.nav_assessment", group: "tools", toolGroup: "calculate", feature: "assessment", element: $("assessment-page"), controller: assessment },
      { id: "blueprints", label: "web.nav_blueprints", group: "tools", toolGroup: "calculate", feature: "blueprints", element: $("blueprint-page"), controller: blueprints },
      { id: "rewind", label: "web.nav_rewind", group: "tools", toolGroup: "calculate", feature: "rewind", element: $("rewind-page"), controller: rewind },
      { id: "warehouse_rules", label: "web.advanced_rules", group: "tools", toolGroup: "organize", feature: "warehouse_rules", element: $("warehouse-rules-page"), controller: rules },
      { id: "catalog", label: "web.nav_catalog", group: "tools", toolGroup: "organize", feature: "catalog", element: $("catalog-page"), controller: catalog },
      { id: "settings", label: "web.nav_settings", element: $("settings-page"), controller: preferences },
    ],
  });
  return { ...navigation, sync() { navigation.sync(); tidy.sync(); }, saveSoftwarePlan, openLibrary: (id) => { library.selectRole(id); navigation.go("library"); }, openTidy: () => tidy.open() };
}
