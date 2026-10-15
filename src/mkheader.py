import sys, json
src, dst = sys.argv[1], sys.argv[2]
t = open(src, encoding='ascii').read()
lm = json.load(open(src[:-4], encoding='utf-8'))['loopMax']
BS = chr(92); Q = chr(34)
out = ['// Genere par mkbanner.py : bandeau IA du lobby local (WMD_LocalLobby.bkm) adapte a WMD_OnlineLobby.bkm',
       '#define BANNER_LOOPMAX %d' % lm, 'static const char BANNER_FRAG[] =']
for i in range(0, len(t), 200):
    c = t[i:i+200].replace(BS, BS + BS).replace(Q, BS + Q)
    out.append('  ' + Q + c + Q)
out[-1] += ';'
open(dst, 'w', encoding='ascii').write('\n'.join(out) + '\n')
print('ok', len(t), lm)
