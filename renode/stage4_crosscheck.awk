# Usage: awk -f renode/stage4_crosscheck.awk logs/stage4_four_mcu*.log
# Compares, at each report time, every receiver's accepted count with the sender's tx count.
/ t=[0-9]+ tx / { node=substr($1,4); t=substr($2,3); tp[node","t]=substr($4,3); th[node","t]=substr($5,3); cur[FILENAME]=t; next }
/ rx / { r=substr($1,4); t=cur[FILENAME];
         for (i=3; i<=NF; i+=3) { s=substr($i,2); split(substr($(i+1),3),p,"/"); split(substr($(i+2),3),h,"/"); key[++n]=r","s","t; ap[n]=p[1]; ah[n]=h[1] } }
END { bad=0; for (k=1; k<=n; k++) { split(key[k],f,","); r=f[1]; s=f[2]; t=f[3];
        if ((s","t) in tp) { if (ap[k]!=tp[s","t] || ah[k]!=th[s","t]) { bad++; print "MISMATCH rx" r " from " s " t=" t ": p " ap[k] " vs " tp[s","t] ", h " ah[k] " vs " th[s","t] } else ok++ } }
      print ok " receiver/sender pairs match, " bad " mismatch" }
