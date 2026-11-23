# FableCAD su macOS

## Interfaccia e pacchetto Mac

Il progetto adotta il nome, il logo e le icone di FableCAD. Le modifiche includono la barra degli strumenti, i comandi e i pannelli di lavoro, la traduzione italiana e la configurazione del bundle macOS. Il DMG ARM64 della versione 2.2.9 è stato firmato, notarizzato da Apple e provato sul Mac di sviluppo.

## Apertura e salvataggio DWG

Il bridge sperimentale usa acadrust 0.6.8. Lo script `libraries/acadrust_bridge/build-experimental.sh` prepara l'eseguibile. Imposta `FABLECAD_ACADRUST_HELPER` al percorso del bridge per abilitarlo.

Il bridge conserva il documento DWG di partenza e verifica il file temporaneo prima di sostituire la destinazione. Le prove hanno coperto geometrie 2D, testi e quote, livelli, blocchi semplici e tratteggi solidi e a motivo. La conservazione delle definizioni dei blocchi dinamici è stata verificata nei campioni utilizzati. Sono stati controllati anche i dati opachi di una DATATABLE DWG 2004.

Per AutoCAD 2020 il formato di riferimento è DWG 2018. I controlli sono stati effettuati con LibreDWG e ODA su un insieme di campioni. La verifica diretta in AutoCAD 2020 è ancora da fare.

## Aggiornamenti

Il controllo delle release GitHub può avvisare all'avvio o dal menu quando è disponibile un tag più recente. Su macOS, Sparkle 2 può installare un aggiornamento differenziale quando il feed offre una patch adatta alla versione installata.

La configurazione CMake opzionale usa `FABLECAD_SPARKLE_FRAMEWORK`, `FABLECAD_SPARKLE_FEED_URL` e `FABLECAD_SPARKLE_PUBLIC_KEY`. Specifica il percorso di Sparkle.framework, l'indirizzo HTTPS dell'appcast e la chiave pubblica EdDSA.

La prova locale 2.2.9 → 2.2.10 ha verificato l'applicazione della patch, la corrispondenza del bundle risultante e il rifiuto di una patch alterata. Restano da verificare il feed HTTPS di produzione e il ciclo completo di installazione e riavvio dall'app firmata per la distribuzione.
