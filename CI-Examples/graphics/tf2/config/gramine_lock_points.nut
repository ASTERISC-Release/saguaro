// Keep every control point locked, including after waiting-for-players causes
// a round reset. This leaves bots free to fight and respawn indefinitely while
// making a control-point victory impossible.
function GramineLockControlPoints() {
    Convars.SetValue("mp_waitingforplayers_cancel", 1);
    EntFire("team_control_point", "SetLocked", "1", 0.0, null);
}

function OnGameEvent_teamplay_round_start(event) {
    GramineLockControlPoints();
}

__CollectGameEventCallbacks(this);
GramineLockControlPoints();
