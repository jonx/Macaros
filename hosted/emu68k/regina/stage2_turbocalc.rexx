/* Deterministic real-application proof for the emu68k public-port bridge. */
options failat 0
options results
resultfile = 'MacRW:Regina68k/stage2.result'

address TCALC

put 10 A1
put 1.25 A2
selectcell A3
put '=A1+A2'

getformula
formula = result
if formula <> '=A1+A2' then do
    call lineout resultfile, 'STAGE2-FAIL formula='formula, 1
    call lineout resultfile
    exit 20
end

getvalue A3
value = result
if value <> '11.25' then do
    call lineout resultfile, 'STAGE2-FAIL value='value, 1
    call lineout resultfile
    exit 21
end

call lineout resultfile, 'STAGE2-PASS TCALC formula='formula' value='value, 1
call lineout resultfile
exit 0
