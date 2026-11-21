; benchmark generated from python API
(set-info :status unknown)
(declare-fun capacity () Int)
(declare-fun snapshot () Int)
(declare-fun local () Int)
(declare-fun read () Int)
(declare-fun published () Int)
(assert
 (let ((?x19 (- local snapshot)))
 (let (($x20 (<= ?x19 capacity)))
 (let (($x17 (>= snapshot 0)))
 (let (($x16 (<= published local)))
 (let (($x15 (<= read published)))
 (let (($x14 (>= read 0)))
 (let (($x13 (> capacity 0)))
 (and $x13 $x14 $x15 $x16 $x17 (<= snapshot read) $x20)))))))))
(assert
 true)
(assert
 (let (($x15 (<= read published)))
 (let (($x49 (and $x15 (<= (- published snapshot) capacity))))
 (not $x49))))
(check-sat)
