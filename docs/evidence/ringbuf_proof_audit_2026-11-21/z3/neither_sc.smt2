; benchmark generated from python API
(set-info :status unknown)
(declare-fun writer_saw_waiting () Bool)
(declare-fun reader_saw_publication () Bool)
(declare-fun lost_wakeup_candidate () Bool)
(assert
 (let (($x65 (not writer_saw_waiting)))
 (let (($x50 (not reader_saw_publication)))
 (let (($x77 (and $x50 $x65)))
 (=> lost_wakeup_candidate $x77)))))
(assert lost_wakeup_candidate)
(check-sat)
