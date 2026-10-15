# Génère le fragment JSON à injecter dans WMD_OnlineLobby.bkm : pour chaque ligne 01..06, copie du
# sous-arbre MNU_AI_0N (sélecteur de niveau IA) et de BTN_RemovePlayer du lobby local (WMD_LocalLobby.bkm),
# rattachés à MNU_LocalPlayer_0N de l'écran en ligne, décalés vers la position de la ligne en ligne.
import json, sys, copy
LOCAL, ONLINE, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
lo = json.load(open(LOCAL, encoding='utf-8'))['scene']['list']
on_doc = json.load(open(ONLINE, encoding='utf-8')); on = on_doc['scene']['list']

def index(L):
    byid, kids, byname = {}, {}, {}
    for k, v in L.items():
        g = v['general']; byid[g['m_id']] = (k, v)
        kids.setdefault(g['m_ParentID'], []).append(g['m_id'])
        byname.setdefault(g['m_Name'], []).append(g['m_id'])
    return byid, kids, byname
lbyid, lkids, lbyname = index(lo)
obyid, okids, obyname = index(on)

def subtree(root):
    out = [root]
    for c in lkids.get(root, []): out += subtree(c)
    return out

def child_named(parent, name):
    for c in lkids.get(parent, []):
        if lbyid[c][1]['general']['m_Name'] == name: return c
    return None

lowc = {}
next_key = on_doc['scene']['loopMax']
frag = {}
refs_out = set()
for n in range(1, 7):
    s = '%02d' % n
    lslot = lbyname['MNU_OfflinePlayerSlot_' + s][0]
    lplayer = lbyname['MNU_OfflinePlayer_' + s][0]
    oslot = obyname['MNU_PlayerSlot_' + s][0]
    oplayer = [i for i in obyname['MNU_LocalPlayer_' + s]][0]
    lp = lbyid[lslot][1]['general']['m_FinalPos']; op = obyid[oslot][1]['general']['m_FinalPos']
    dx, dy = op['x'] - lp['x'], op['y'] - lp['y']
    roots = [lbyname['MNU_AI_' + s][0]]
    rm = child_named(lplayer, 'BTN_RemovePlayer')
    if rm: roots.append(rm)
    # v69 : bouton « Ajouter l'IA » (MNU_OfflineAddAI_0N, enfant du slot) ; son activateur (0x8BE840, générique :
    # visible sur la 1re case libre de la liste DataParent) est le même que celui de MNU_AddPlayer en ligne
    if lbyname.get('MNU_OfflineAddAI_' + s): roots.append(lbyname['MNU_OfflineAddAI_' + s][0])
    ids = []
    for r in roots: ids += subtree(r)
    remap = {}
    for i in ids:   # v68 : la scène indexe par le mot BAS (table de 9999 entrées) -> compteur unique 0x0C01.. (> 0xBE3)
        lowc['n'] = lowc.get('n', 0x0C00) + 1
        assert lowc['n'] < 0x270F
        remap[i] = (i & 0xFFFF0000) | lowc['n']
    for i in ids:
        k, v = lbyid[i]; w = copy.deepcopy(v); g = w['general']
        g['m_id'] = remap[i]
        lpar = g['m_ParentID']
        g['m_ParentID'] = remap[lpar] if lpar in remap else (oslot if lpar == lslot else oplayer)
        g['m_Name'] = g['m_Name'] + '_IAMOD'
        for key in ('m_FinalPos',):
            g[key]['x'] += dx; g[key]['y'] += dy
        if lbyid[i][1]['general']['m_Name'].startswith('MNU_OfflineAddAI_'):
            # en ligne « Ajouter un joueur » occupe x=215 (place de l'IA en local) -> IA à x=-15 (place du joueur en local)
            g['m_Rect']['x'] -= 230
        if 'AddAI' in lbyid[i][1]['general']['m_Name']:
            w['flags']['m_bInvisible'] = False   # v70 : comme MNU/BTN_AddPlayer en ligne (cadre du bouton visible)
            # v71 : couche et police comme MNU_AddPlayer (7) / BTN_AddPlayer (9) en ligne : en couche 8 le cadre est
            # dessiné sous un fond de l'écran en ligne (le texte, passe à part, restait visible)
            g['m_Layer'] += 1
            if 'button' in w: w['button']['m_FontID'] = obyid[obyname['BTN_AddPlayer_01'][0]][1]['button']['m_FontID']
        if lbyid[i][1]['general']['m_Name'] == 'BTN_RemovePlayer':
            # v73 : la croix n'a pas d'activateur (visible sur toute ligne de joueur LOCAL, ex. la sienne chez
            # l'invité) -> conteneur MNU copié de MNU_AI (activateur « Is AI » 0x2AFD0032, même DataParent),
            # champ retiré, placé à la position de la croix ; la croix y est rattachée en (0,0)
            ai = [x for x in frag.values() if x['general']['m_Name'] == 'MNU_AI_' + s + '_IAMOD'][0]
            box = copy.deepcopy(ai); bg = box['general']
            lowc['n'] += 1; assert lowc['n'] < 0x270F
            bg['m_id'] = 0x1B5A0000 | lowc['n']; bg['m_Name'] = 'MNU_RemoveAI_' + s + '_IAMOD'
            bg['m_ParentID'] = g['m_ParentID']
            bg['m_Rect'] = dict(g['m_Rect']); bg['m_Rect']['x'] -= 46; bg['m_FinalPos'] = dict(g['m_FinalPos']); bg['m_FinalPos']['x'] -= 46
            bg['m_Layer'] = max(0, g['m_Layer'] - 1)
            box['nds']['m_NDSID']['m_FieldID'] = 0
            for kk in list(box['focus']):
                if kk.startswith('m_FocusLink') and kk.endswith('ID'): box['focus'][kk] = 0
            frag[str(next_key)] = box; next_key += 1
            g['m_ParentID'] = bg['m_id']; g['m_Rect']['x'] = 0.0; g['m_Rect']['y'] = 0.0
            g['m_Rect']['x'] += 46   # compensé par le -46 ci-dessous
            # v66 : la position réelle est m_Rect (relative au parent) ; m_FinalPos n'est qu'un cache
            g['m_Rect']['x'] -= 46; g['m_FinalPos']['x'] -= 46
        nd = w.get('nds', {}).get('m_NDSID', {})
        dp = nd.get('m_DataParentID', 0)
        if dp:
            if dp in remap: nd['m_DataParentID'] = remap[dp]
            elif dp == lplayer: nd['m_DataParentID'] = oplayer
            elif dp == lslot: nd['m_DataParentID'] = oslot
            elif dp == lbyname['SB_OfflinePlayers'][0]: nd['m_DataParentID'] = obyname['SB_OnlinePlayers'][0]
            else: refs_out.add(('DataParent', g['m_Name'], dp))
        # autres références à des identifiants
        def scan(o, path=''):
            if isinstance(o, dict):
                for kk, vv in o.items():
                    if isinstance(vv, int) and vv >= 458000000 and kk not in ('m_id', 'm_ParentID', 'm_FontID', 'm_ResID', 'm_DataParentID', 'm_FieldID', 'm_ActivatorID', 'm_MonitorID'):
                        if vv in remap: o[kk] = remap[vv]
                        elif kk.startswith('m_FocusLink'): o[kk] = 0
                        elif kk == 'm_NDSLocalFieldID': pass
                        else: refs_out.add((kk, g['m_Name'], vv))
                    else: scan(vv, path + '.' + kk)
            elif isinstance(o, list):
                for x in o: scan(x, path)
        scan(w)
        frag[str(next_key)] = w; next_key += 1
onlow = {v['general']['m_id'] & 0xFFFF for v in on.values()}
newlow = [v['general']['m_id'] & 0xFFFF for v in frag.values()]
assert len(set(newlow)) == len(newlow) and not (set(newlow) & onlow), 'collision mot bas'
print('mots bas %04X..%04X (max ecran %04X)' % (min(newlow), max(newlow), max(onlow)))
json.dump({'frag': frag, 'loopMax': next_key}, open(OUT, 'w', encoding='utf-8'), ensure_ascii=False)
txt = ','.join('"%s":%s' % (k, json.dumps(v, ensure_ascii=True, separators=(',', ':'))) for k, v in frag.items())
open(OUT + '.txt', 'w', encoding='ascii').write(txt)
print('texte', len(txt))
print('elements', len(frag), 'loopMax', next_key)
for r in sorted(refs_out, key=str)[:40]: print('REF HORS ARBRE', r)
