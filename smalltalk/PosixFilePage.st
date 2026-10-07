FilePage subclass: #PosixFilePage
	instanceVariableNames: 'pageNumber bytesInPage '
	classVariableNames: ''
	poolDictionaries: ''
	category: 'Files-Posix'!


!PosixFilePage methodsFor: 'file/page/directory implementation'!

pageNumber: pn
	pageNumber _ pn! !

!PosixFilePage methodsFor: 'page accessing'!

pageNumber
	^pageNumber! !

!PosixFilePage methodsFor: 'accessing'!

serialNumber: n
	^self!

size
	^bytesInPage!

size: size
	bytesInPage _ size! !
