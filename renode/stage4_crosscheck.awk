# Usage: awk -f renode/stage4_crosscheck.awk logs/mcu*.log
# Compares, at each 1 s report, every receiver's accepted frame count with the sender's sent count.
# Reports are matched by their second (t=5 is report 0, t=1005 or 1006 is report 1), so a report
# that ran a millisecond late still pairs up; nothing is sent between x005 and x009 ms.
# Frames a receiver missed before its first report (it booted later than the sender, as MCU1 does
# in Renode's lockstep start) are listed as boot losses. After that the difference between sent
# and accepted must never grow: any later loss is a mismatch.
/ t=[0-9]+ tx / { node=substr($1,4); k=int((substr($2,3)+500)/1000); tp[node","k]=substr($4,3); th[node","k]=substr($5,3); cur[FILENAME]=k; next }
/ rx n[0-9]/ { r=substr($1,4); k=cur[FILENAME];
         for (i=3; i<=NF; i+=4) { s=substr($i,2); split(substr($(i+1),3),p,"/"); split(substr($(i+2),3),h,"/"); key[++n]=r","s","k; ap[n]=p[1]; ah[n]=h[1] } }
END { bad=0; ok=0; boot=0
      for (j=1; j<=n; j++) { split(key[j],f,","); r=f[1]; s=f[2]; k=f[3]
          if (!((s","k) in tp)) continue
          dp=tp[s","k]-ap[j]; dh=th[s","k]-ah[j]
          if (!((r","s) in basep)) { basep[r","s]=dp; baseh[r","s]=dh; if (dp!=0 || dh!=0) { boot++; print "BOOT LOSS rx" r " from " s ": " dp " blocks, " dh " heartbeats before its first report" } }
          if (dp!=basep[r","s] || dh!=baseh[r","s]) { bad++; print "MISMATCH rx" r " from " s " report " k ": p " ap[j] " vs " tp[s","k] ", h " ah[j] " vs " th[s","k] } else ok++ }
      print ok " receiver/sender pairs match, " bad " mismatch, " boot " pairs with boot losses" }
