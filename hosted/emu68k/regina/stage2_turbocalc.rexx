/* Deterministic real-application proof for the emu68k public-port bridge. */
options failat 0
options results
resultfile = 'MacRW:Regina68k/stage2.result'

address TCALC

call lineout resultfile, 'STEP getcursorpos', 1
getcursorpos
oldpos = result
call lineout resultfile, 'STEP put A1', 1
put 10 A1
call lineout resultfile, 'STEP put A2'
put 1.25 A2
call lineout resultfile, 'STEP select A3'
selectcell 'A3'
call lineout resultfile, 'STEP put formula'
put '=A1+A2'

call lineout resultfile, 'STEP getformula'
getformula
formula = result
if formula <> '=A1+A2' then do
    call lineout resultfile, 'STAGE2-FAIL formula='formula, 1
    call lineout resultfile
    exit 20
end

call lineout resultfile, 'STEP getvalue'
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
