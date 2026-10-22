# Genera ns_smnet_lowpoly.c (python3 lowpoly.py ../sm64coopdx/src/pc/network/smnet/ns_smnet_lowpoly.c): un Mario de cajas con el mismo esqueleto (20 partes animadas)
import itertools, math, sys
# nombre, parte de color, x0, x1, y (centro), ry, z (centro), rz
parts = {
 'butt':     [('PANTS', -10, 75, 0, 46, 0, 52)],
 'torso':    [('SHIRT',  -5, 70, 0, 44, 0, 54), ('PANTS', -5, 40, 0, 46, 0, 50)],
 'head':     [('SKIN',  -10, 105, 0, 62, 0, 62), ('CAP', 75, 150, 0, 70, 0, 70)],
 'arm':      [('SHIRT',  -8, 70, 0, 20, 0, 20)],
 'forearm':  [('SHIRT',  -6, 64, 0, 18, 0, 18)],
 'hand':     [('GLOVES', -6, 46, 0, 26, 0, 24)],
 'thigh':    [('PANTS', -10, 95, 0, 30, 0, 30)],
 'leg':      [('PANTS',  -8, 72, 0, 26, 0, 26)],
 'foot':     [('SHOES', -10, 44, 0, 32, 0, 28)],
}
out = ['/* generado por herramientas/lowpoly.py: Mario de pocas caras para los impostores bajos y lejanos.',
       '   Mismo esqueleto que mario_geo_body (20 partes animadas, mismo orden) para que funcionen',
       '   las animaciones; cada parte es una caja con normales suavizadas y el color de la paleta. */',
       '#ifdef SMNET', '#include <ultra64.h>', '#include "sm64.h"', '#include "geo_commands.h"',
       '#include "game/mario_misc.h"', '#include "game/player_palette.h"', '#include "ns_smnet_lowpoly.h"', '#include <stdint.h>', '',
       '/* DynOS mide las listas que no son de vanilla buscando su centinela (gfx_get_length, la que',
       '   pone gfx_allocate_internal): sin ella sigue de largo por la memoria y crashea */',
       '#define LP_SENTINEL {{ _SHIFTL(G_ENDDL, 24, 8) | _SHIFTL(UINT32_MAX, 0, 24), UINT32_MAX }}',
       '/* sin const: DynOS corrige en el lugar los punteros de las listas de dibujo al cargar el modelo',
       '   (en Windows, const va a memoria de solo lectura y la escritura crashea) */', '']
faces = [((0,2,6,4),(-1,0,0)),((1,5,7,3),(1,0,0)),((0,4,5,1),(0,-1,0)),((2,3,7,6),(0,1,0)),((0,1,3,2),(0,0,-1)),((4,6,7,5),(0,0,1))]
def corner(i, b):
    _, x0, x1, yc, ry, zc, rz = b
    bx, by, bz = (i >> 2) & 1, (i >> 1) & 1, i & 1
    return (x1 if bx else x0, yc + (ry if by else -ry), zc + (rz if bz else -rz)), (1 if bx else -1, 1 if by else -1, 1 if bz else -1)
def sub(a, b): return [a[i]-b[i] for i in range(3)]
def cross(a, b): return [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]]
def dot(a, b): return sum(a[i]*b[i] for i in range(3))
tris_total = 0
for name, boxes in parts.items():
    for bi, b in enumerate(boxes):
        vs = []
        for i in range(8):
            p, s = corner(i, b)
            n = [s[0]*0.5, s[1], s[2]]; l = math.sqrt(dot(n, n)); n = [int(round(c / l * 120)) for c in n]
            vs.append('    {{{ %d, %d, %d }, 0, { 0, 0 }, { %d, %d, %d, 255 }}},' % (p[0], p[1], p[2], n[0], n[1], n[2]))
        out.append('static Vtx lp_%s_%d_v[] = {' % (name, bi)); out += vs; out.append('};')
    out.append('static Gfx lp_%s_dl[] = {' % name)
    out.append('    gsDPPipeSync(),')
    out.append('    gsDPSetCombineMode(G_CC_SHADE, G_CC_SHADE),')
    out.append('    gsSPClearGeometryMode(G_TEXTURE_GEN),')
    out.append('    gsSPSetGeometryMode(G_LIGHTING | G_SHADE | G_SHADING_SMOOTH | G_CULL_BACK),')
    out.append('    gsSPNumLights(NUMLIGHTS_1),')
    for bi, b in enumerate(boxes):
        out.append('    gsSPCopyLightsPlayerPart(%s),' % b[0])
        out.append('    gsSPVertex(lp_%s_%d_v, 8, 0),' % (name, bi))
        tl = []
        pts = [corner(i, b)[0] for i in range(8)]
        for (q, nrm) in faces:
            for t in ((q[0], q[1], q[2]), (q[0], q[2], q[3])):
                c = cross(sub(pts[t[1]], pts[t[0]]), sub(pts[t[2]], pts[t[0]]))
                if dot(c, nrm) < 0: t = (t[0], t[2], t[1])
                tl.append(t)
        tris_total += len(tl)
        for k in range(0, len(tl), 2):
            a, c = tl[k], tl[k+1]
            out.append('    gsSP2Triangles(%d, %d, %d, 0x0, %d, %d, %d, 0x0),' % (a + c))
    out.append('    gsSPEndDisplayList(),'); out.append('    LP_SENTINEL,'); out.append('};'); out.append('')
geo = '''extern const Gfx mario_material_revert_render_settings[];
static const GeoLayout lp_body[] = {
   GEO_ANIMATED_PART(LAYER_OPAQUE, 0, 0, 0, NULL),
   GEO_OPEN_NODE(),
      GEO_ANIMATED_PART(LAYER_OPAQUE, 0, 0, 0, lp_butt_dl),
      GEO_OPEN_NODE(),
         GEO_ANIMATED_PART(LAYER_OPAQUE, 68, 0, 0, lp_torso_dl),
         GEO_OPEN_NODE(),
            GEO_ANIMATED_PART(LAYER_OPAQUE, 87, 0, 0, lp_head_dl),
            GEO_ANIMATED_PART(LAYER_OPAQUE, 67, -10, 79, NULL),
            GEO_OPEN_NODE(),
               GEO_ANIMATED_PART(LAYER_OPAQUE, 0, 0, 0, lp_arm_dl),
               GEO_OPEN_NODE(),
                  GEO_ANIMATED_PART(LAYER_OPAQUE, 65, 0, 0, lp_forearm_dl),
                  GEO_OPEN_NODE(),
                     GEO_ANIMATED_PART(LAYER_OPAQUE, 60, 0, 0, lp_hand_dl),
                  GEO_CLOSE_NODE(),
               GEO_CLOSE_NODE(),
            GEO_CLOSE_NODE(),
            GEO_ANIMATED_PART(LAYER_OPAQUE, 68, -10, -79, NULL),
            GEO_OPEN_NODE(),
               GEO_ANIMATED_PART(LAYER_OPAQUE, 0, 0, 0, lp_arm_dl),
               GEO_OPEN_NODE(),
                  GEO_ANIMATED_PART(LAYER_OPAQUE, 65, 0, 0, lp_forearm_dl),
                  GEO_OPEN_NODE(),
                     GEO_ANIMATED_PART(LAYER_OPAQUE, 60, 0, 0, lp_hand_dl),
                  GEO_CLOSE_NODE(),
               GEO_CLOSE_NODE(),
            GEO_CLOSE_NODE(),
         GEO_CLOSE_NODE(),
         GEO_ANIMATED_PART(LAYER_OPAQUE, 13, -8, 42, NULL),
         GEO_OPEN_NODE(),
            GEO_ANIMATED_PART(LAYER_OPAQUE, 0, 0, 0, lp_thigh_dl),
            GEO_OPEN_NODE(),
               GEO_ANIMATED_PART(LAYER_OPAQUE, 89, 0, 0, lp_leg_dl),
               GEO_OPEN_NODE(),
                  GEO_ANIMATED_PART(LAYER_OPAQUE, 67, 0, 0, lp_foot_dl),
               GEO_CLOSE_NODE(),
            GEO_CLOSE_NODE(),
         GEO_CLOSE_NODE(),
         GEO_ANIMATED_PART(LAYER_OPAQUE, 13, -8, -42, NULL),
         GEO_OPEN_NODE(),
            GEO_ANIMATED_PART(LAYER_OPAQUE, 0, 0, 0, lp_thigh_dl),
            GEO_OPEN_NODE(),
               GEO_ANIMATED_PART(LAYER_OPAQUE, 89, 0, 0, lp_leg_dl),
               GEO_OPEN_NODE(),
                  GEO_ANIMATED_PART(LAYER_OPAQUE, 67, 0, 0, lp_foot_dl),
               GEO_CLOSE_NODE(),
            GEO_CLOSE_NODE(),
         GEO_CLOSE_NODE(),
      GEO_CLOSE_NODE(),
   GEO_CLOSE_NODE(),
   GEO_RETURN(),
};

const GeoLayout ns_smnet_lowpoly_geo[] = {
   GEO_NODE_START(),
   GEO_OPEN_NODE(),
      GEO_SHADOW(SHADOW_CIRCLE_PLAYER, 0xB4, 100),
      GEO_OPEN_NODE(),
         GEO_SCALE(0x00, 16384),
         GEO_OPEN_NODE(),
            GEO_ASM(LAYER_OPAQUE + 3, geo_mario_set_player_colors),
            GEO_BRANCH(1, lp_body),
         GEO_CLOSE_NODE(),
      GEO_CLOSE_NODE(),
      GEO_DISPLAY_LIST(LAYER_OPAQUE, mario_material_revert_render_settings),
   GEO_CLOSE_NODE(),
   GEO_END(),
};
#endif
'''
out.append(geo)
open(sys.argv[1], 'w').write('\n'.join(out))
print('triángulos:', tris_total)
