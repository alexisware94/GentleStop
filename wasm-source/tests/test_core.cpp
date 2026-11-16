#include "../gentlestop_core.h"
#include <cstdio>
int main() {
    gs::Controller c; double dz, sm, jr, thl, spl; int en, inv, sl, spo, tho, cv;
    if (scanf("%d %d %d %d %lf %d %lf %lf %d %lf %lf", &en,&inv,&sl,&spo,&spl,&tho,&thl,&dz,&cv,&sm,&jr) != 11) return 1;
    c.s.enabled=en; c.s.invert=inv; c.s.strobeLock=sl; c.s.speedLimitOn=spo; c.s.speedLimitKt=spl;
    c.s.throttleLimitOn=tho; c.s.throttleLimit=thl; c.s.deadzone=dz; c.s.curve=cv; c.s.smoothing=sm; c.s.jumpResponse=jr; c.s.clamp();
    gs::Inputs in; double dt;
    while (scanf("%lf %lf %lf %lf %lf %lf %lf %lf", &in.yokeY,&in.onGround,&in.airspeedKt,&in.strobe,&in.throttle1,&in.throttle2,&in.parking,&dt) == 8) {
        c.step(in, dt);
        printf("%.17g %d %d %d\n", c.brake, c.gate?1:0, (int)c.reason, gs::Controller::axisValue(c.brake>0.005?c.brake:0.0));
    }
}
