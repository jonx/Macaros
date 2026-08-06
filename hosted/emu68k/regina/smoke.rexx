/* Deterministic Regina workload shared by native AROS and m68k emu68k runs. */
say 'REGINA-SMOKE-BEGIN'

numeric digits 20
sum = 0
fact = 1
do i = 1 to 1000
    sum = sum + i
    if i <= 20 then fact = fact * i
end
say 'SUM='sum
say 'FACT20='fact

text = 'alpha beta gamma delta'
parse var text first second rest
say 'PARSE='first'|'second'|'rest
say 'REVERSE='reverse('AmigaOS-68k')
say 'TRANSLATE='translate('bridge waterline')

fib0 = 0
fib1 = 1
fibline = fib0 fib1
do i = 3 to 18
    next = fib0 + fib1
    fibline = fibline next
    fib0 = fib1
    fib1 = next
end
say 'FIB='fibline

say 'REGINA-SMOKE-END'
exit 0
