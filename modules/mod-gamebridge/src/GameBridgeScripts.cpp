// Script hook registrations for mod-gamebridge. Every hook forwards to the Bridge,
// which filters on the bridge's own session/player/entities.
#include "GameBridge.h"

#include "AllSpellScript.h"
#include "ScriptMgr.h"
#include "ServerScript.h"
#include "UnitScript.h"
#include "WorldScript.h"

using GameBridge::Bridge;

class GameBridgeWorldScript : public WorldScript
{
public:
    GameBridgeWorldScript() : WorldScript("GameBridgeWorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_SHUTDOWN }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        if (!reload)   // the session/endpoint are not reconfigured live
            Bridge::Instance().LoadConfig();
    }

    // Runs in World::Update after MapMgr::Update has joined all map threads.
    void OnUpdate(uint32 diff) override { Bridge::Instance().Update(diff); }

    // Runs on the main thread after the world loop stopped, before maps are unloaded.
    void OnShutdown() override { Bridge::Instance().Shutdown(); }
};

class GameBridgeServerScript : public ServerScript
{
public:
    GameBridgeServerScript() : ServerScript("GameBridgeServerScript", { SERVERHOOK_ON_PACKET_SENT }) { }

    // WorldSession::SendPacket invokes this before its null-socket early return.
    void OnPacketSent(WorldSession* session, WorldPacket const& packet) override
    {
        Bridge::Instance().HookPacketSent(session, packet);
    }
};

class GameBridgeSpellScript : public AllSpellScript
{
public:
    GameBridgeSpellScript() : AllSpellScript("GameBridgeSpellScript", {
        ALLSPELLHOOK_ON_PREPARE, ALLSPELLHOOK_ON_CAST, ALLSPELLHOOK_ON_CAST_CANCEL }) { }

    void OnSpellPrepare(Spell* spell, Unit* caster, SpellInfo const* /*spellInfo*/) override
    {
        Bridge::Instance().HookSpellPrepare(spell, caster);
    }

    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* /*spellInfo*/, bool /*skipCheck*/) override
    {
        Bridge::Instance().HookSpellCast(spell, caster);
    }

    void OnSpellCastCancel(Spell* spell, Unit* caster, SpellInfo const* /*spellInfo*/, bool /*bySelf*/) override
    {
        Bridge::Instance().HookSpellCancel(spell, caster);
    }
};

class GameBridgeUnitScript : public UnitScript
{
public:
    // DealDamage is dispatched to every registered UnitScript (it has no hook id);
    // a non-empty list keeps the other hooks from being enabled for nothing.
    GameBridgeUnitScript() : UnitScript("GameBridgeUnitScript", true, { UNITHOOK_ON_UNIT_DEATH }) { }

    uint32 DealDamage(Unit* attacker, Unit* victim, uint32 damage, DamageEffectType /*damagetype*/) override
    {
        return Bridge::Instance().HookDealDamage(attacker, victim, damage);
    }
};

void AddGameBridgeScripts()
{
    new GameBridgeWorldScript();
    new GameBridgeServerScript();
    new GameBridgeSpellScript();
    new GameBridgeUnitScript();
}
