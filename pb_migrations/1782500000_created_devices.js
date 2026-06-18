/// <reference path="../pb_data/types.d.ts" />
// Geräte-Registry: jeder ESP registriert sich (chipId) und liest seine Rolle.
// Im Web (Gerät-Tab von Kalk & Nährstoff) wird die Rolle zugewiesen/umgeschaltet.
migrate((app) => {
  const rule = "@request.auth.id != '' && @request.auth.verified = true && user = @request.auth.id";
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
  const fCreated = { "hidden": false, "id": "autodate2990389176", "name": "created", "onCreate": true, "onUpdate": false, "presentable": false, "system": false, "type": "autodate" };
  const fUpdated = { "hidden": false, "id": "autodate3332085495", "name": "updated", "onCreate": true, "onUpdate": true, "presentable": false, "system": false, "type": "autodate" };
  const txt = (id, name, req) => ({ "autogeneratePattern": "", "help": "", "hidden": false, "id": id, "max": 0, "min": 0, "name": name, "pattern": "", "presentable": false, "primaryKey": false, "required": !!req, "system": false, "type": "text" });
  const num = (id, name) => ({ "help": "", "hidden": false, "id": id, "max": null, "min": null, "name": name, "onlyInt": false, "presentable": false, "required": false, "system": false, "type": "number" });

  let exists = null;
  try { exists = app.findCollectionByNameOrId("devices"); } catch (e) { exists = null; }
  if (!exists) {
    const c = new Collection({
      "type": "base",
      "name": "devices",
      "system": false,
      "listRule": rule, "viewRule": rule, "createRule": rule, "updateRule": rule, "deleteRule": rule,
      "fields": [
        fId, fUser,
        txt("text7001000001", "chipId", true),
        txt("text7001000002", "role", false),
        txt("text7001000003", "name", false),
        num("number7001000004", "lastSeen"),
        txt("text7001000005", "fwVersion", false),
        fCreated, fUpdated
      ],
      "indexes": ["CREATE UNIQUE INDEX idx_devices_user_chip ON devices (user, chipId)"]
    });
    app.save(c);
  }
}, (app) => {
  try { app.delete(app.findCollectionByNameOrId("devices")); } catch (e) {}
});
