
out vec4 sk_FragColor;
void fn1_v() {
    sk_FragColor.x = 0.0;
}
void fn2_v() {
    fn1_v();
    fn1_v();
    fn1_v();
}
void fn3_v() {
    fn2_v();
    fn2_v();
    fn2_v();
}
void fn4_v() {
    fn3_v();
    fn3_v();
    fn3_v();
}
void fn5_v() {
    fn4_v();
    fn4_v();
    fn4_v();
}
void fn6_v() {
    fn5_v();
    fn5_v();
    fn5_v();
}
void fn7_v() {
    fn6_v();
    fn6_v();
    fn6_v();
}
void fn8_v() {
    fn7_v();
    fn7_v();
    fn7_v();
}
void fn9_v() {
    fn8_v();
    fn8_v();
    fn8_v();
}
void fnA_v() {
    fn9_v();
    fn9_v();
    fn9_v();
}
void fnB_v() {
    fnA_v();
    fnA_v();
    fnA_v();
}
void fnC_v() {
    fnB_v();
    fnB_v();
    fnB_v();
}
void fnD_v() {
    fnC_v();
    fnC_v();
    fnC_v();
}
void fnE_v() {
    fnD_v();
    fnD_v();
    fnD_v();
}
void fnF_v() {
    fnE_v();
    fnE_v();
    fnE_v();
}
void fnG_v() {
    fnF_v();
    fnF_v();
    fnF_v();
}
void fnH_v() {
    fnG_v();
    fnG_v();
    fnG_v();
}
void fnI_v() {
    fnH_v();
    fnH_v();
    fnH_v();
}
void fnJ_v() {
    fnI_v();
    fnI_v();
    fnI_v();
}
void fnK_v() {
    fnJ_v();
    fnJ_v();
    fnJ_v();
}
void fnL_v() {
    fnK_v();
    fnK_v();
    fnK_v();
}
void fnM_v() {
    fnL_v();
    fnL_v();
    fnL_v();
}
void fnN_v() {
    fnM_v();
    fnM_v();
    fnM_v();
}
void main() {
    fnN_v();
}
