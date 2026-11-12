# Usage: awk -f renode/stage5_params_crosscheck.awk logs/mcu1.log logs/mcu2.log logs/mcu3.log logs/mcu4.log
# At each report from 1 s on, every receiver's copy of P1-P20 must equal the owner's own
# value (raw 4 bytes), and every node must report synced=20. Reports run 5 ms after a send, so
# the last block of every node has arrived and no sample has been taken since. Reports are matched
# by their second (t=1005 or 1006 is report 1): a report a millisecond late still sees the same
# values, because nothing is sampled or sent between x005 and x009 ms. Printed times are x005.
/ t=[0-9]+ tx / { cur[FILENAME]=int((substr($2,3)+500)/1000); next }
/ db synced=/   { syn[substr($1,4)","cur[FILENAME]]=substr($3,8); next }
/ val /         { node=substr($1,4); t=cur[FILENAME]; seen[t]=1; has[node]=1
                  for (i=3; i<=NF; i++) { split($i,kv,"="); v[node","t","kv[1]]=kv[2] } }
END {
    for (t in seen) {
        if (t+0 < 1) continue
        ts=t*1000+5
        for (r in has) {
            if (syn[r","t] != 20) { bad++; print "NOT SYNCED MCU" r " t=" ts ": synced=" syn[r","t] }
            for (id=1; id<=20; id++) {
                owner=int((id-1)/5)+1
                if (owner==r || !((owner","t","id) in v)) continue
                if (v[r","t","id] != v[owner","t","id]) { bad++; print "MISMATCH t=" ts " MCU" r " P" id "=" v[r","t","id] " owner MCU" owner "=" v[owner","t","id] }
                else ok++
            }
        }
    }
    print ok " parameter copies match their owner, " bad+0 " problems"
}
