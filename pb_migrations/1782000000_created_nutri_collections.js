/// <reference path="../pb_data/types.d.ts" />
// Nährstoffmanagement: spiegelt die aqua_*-Struktur nach nutri_*
// (eigener ESP/Subsystem für NO3/PO4, gleiche Owner-Regeln, eigene Index-Namen).
migrate((app) => {
  const rule = "@request.auth.id != '' && @request.auth.verified = true && user = @request.auth.id";

  // gemeinsame System-/Owner-Felder (Feld-IDs dürfen über Collections geteilt werden,
  // genau wie es die aqua_*-Collections bereits tun)
  const fId = {
    "autogeneratePattern": "[a-z0-9]{15}", "help": "", "hidden": false, "id": "text3208210256",
    "max": 15, "min": 15, "name": "id", "pattern": "^[a-z0-9]+$", "presentable": false,
    "primaryKey": true, "required": true, "system": true, "type": "text"
  };
  const fUser = {
    "cascadeDelete": true, "collectionId": "_pb_users_auth_", "help": "", "hidden": false,
    "id": "relation2375276105", "maxSelect": 1, "minSelect": 0, "name": "user",
    "presentable": false, "required": true, "system": false, "type": "relation"
  };
  const fCreated = {
    "hidden": false, "id": "autodate2990389176", "name": "created", "onCreate": true,
    "onUpdate": false, "presentable": false, "system": false, "type": "autodate"
  };
  const fUpdated = {
    "hidden": false, "id": "autodate3332085495", "name": "updated", "onCreate": true,
    "onUpdate": true, "presentable": false, "system": false, "type": "autodate"
  };
  const txt = (id, name, req) => ({
    "autogeneratePattern": "", "help": "", "hidden": false, "id": id, "max": 0, "min": 0,
    "name": name, "pattern": "", "presentable": false, "primaryKey": false,
    "required": !!req, "system": false, "type": "text"
  });
  const num = (id, name) => ({
    "help": "", "hidden": false, "id": id, "max": null, "min": null, "name": name,
    "onlyInt": false, "presentable": false, "required": false, "system": false, "type": "number"
  });
  const bool = (id, name) => ({
    "help": "", "hidden": false, "id": id, "name": name, "presentable": false,
    "required": false, "system": false, "type": "bool"
  });
  const json = (id, name) => ({
    "help": "", "hidden": false, "id": id, "maxSize": 2000000, "name": name,
    "presentable": false, "required": false, "system": false, "type": "json"
  });

  const specs = [
    {
      name: "nutri_docs",
      indexes: ["CREATE UNIQUE INDEX idx_ndocs_user_key ON nutri_docs (user, key)"],
      fields: [fId, fUser, txt("text2324736937", "key", true), json("json2918445923", "data"), fCreated, fUpdated]
    },
    {
      name: "nutri_measurements",
      indexes: ["CREATE INDEX idx_nmeas_user_type_ts ON nutri_measurements (user, type, timestamp)"],
      fields: [fId, fUser, txt("text2363381545", "type", false), num("number494360628", "value"),
               num("number2782324286", "timestamp"), txt("text1602912115", "source", false), fCreated, fUpdated]
    },
    {
      name: "nutri_dosings",
      indexes: ["CREATE INDEX idx_ndos_user_ts ON nutri_dosings (user, timestamp)"],
      fields: [fId, fUser, num("number2551689709", "pump"), num("number3582863974", "ml"),
               num("number2782324286", "timestamp"), bool("bool1090053591", "isAutomatic"),
               num("number3979930624", "factor"), txt("text4274631679", "dosageType", false),
               bool("bool1862328242", "success"), fCreated, fUpdated]
    },
    {
      name: "nutri_command",
      indexes: ["CREATE UNIQUE INDEX idx_ncmd_user ON nutri_command (user)"],
      fields: [fId, fUser, txt("text4115064751", "cmdId", false), txt("text1204587666", "action", false),
               txt("text2063623452", "status", false), num("number2551689709", "pump"),
               num("number3582863974", "ml"), num("number874646130", "steps"),
               json("json325763347", "result"), fCreated, fUpdated]
    }
  ];

  for (let i = 0; i < specs.length; i++) {
    const s = specs[i];
    let exists = null;
    try { exists = app.findCollectionByNameOrId(s.name); } catch (e) { exists = null; }
    if (exists) continue;
    const collection = new Collection({
      "type": "base",
      "name": s.name,
      "system": false,
      "listRule": rule,
      "viewRule": rule,
      "createRule": rule,
      "updateRule": rule,
      "deleteRule": rule,
      "fields": s.fields,
      "indexes": s.indexes
    });
    app.save(collection);
  }
}, (app) => {
  const names = ["nutri_docs", "nutri_measurements", "nutri_dosings", "nutri_command"];
  for (let i = 0; i < names.length; i++) {
    try { app.delete(app.findCollectionByNameOrId(names[i])); } catch (e) {}
  }
});
