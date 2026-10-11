import { useEffect, useState } from 'react';
import { useApp, simNow, updateUi } from './store.js';
import { fmtTime } from './format.js';
import { ConnectSheet } from './ConnectSheet.jsx';
import { DemoPanel } from './DemoPanel.jsx';
import { Guide } from './Guide.jsx';
import { Btn, Icon, Sheet, Toasts, useTick } from './ui.jsx';
import { Placeholder, Products } from './screens/Products.jsx';
import { DeviceHub, FunctionScreen, McoldHome } from './screens/Mcold.jsx';
import { MultiStart } from './screens/MultiStart.jsx';
import { StartTrip, StopTrip } from './screens/TripFlows.jsx';
import { WifiEdit } from './screens/Settings.jsx';
import { SendProgress } from './screens/Send.jsx';

function Route() {
  const { ui } = useApp();
  const r = ui.route[ui.route.length - 1];
  switch (r.name) {
    case 'placeholder':
      return <Placeholder product={r.product} />;
    case 'mcold':
      return <McoldHome />;
    case 'device':
      return <DeviceHub />;
    case 'trip':
    case 'settings':
    case 'send':
      return <FunctionScreen fn={r.name} />;
    case 'multi-start':
      return <MultiStart />;
    case 'start-trip':
      return <StartTrip />;
    case 'stop-trip':
      return <StopTrip />;
    case 'wifi-edit':
      return <WifiEdit slot={r.slot} />;
    case 'send-progress':
      return <SendProgress ids={r.ids} />;
    default:
      return <Products />;
  }
}

function StatusBar() {
  const s = useApp();
  useTick(20000);
  return (
    <div className="statusbar" aria-hidden="true">
      <span className="num">{fmtTime(simNow())}</span>
      <span className="row">
        {s.ble && <Icon name="bluetooth" size={14} stroke={2.2} />}
        <Icon name={s.internet ? 'antenna-bars-5' : 'world-off'} size={15} stroke={2} />
        {s.internet && <Icon name="wifi" size={15} stroke={2} />}
        <Icon name="battery-3" size={17} stroke={1.8} />
      </span>
    </div>
  );
}

const readView = () => (typeof location !== 'undefined' && location.hash === '#guide' ? 'guide' : 'app');

export default function App() {
  const s = useApp();
  const [view, setView] = useState(readView);
  useEffect(() => {
    const h = () => setView(readView());
    window.addEventListener('hashchange', h);
    return () => window.removeEventListener('hashchange', h);
  }, []);
  const openGuide = () => {
    updateUi(() => ({ demoOpen: false }));
    location.hash = 'guide';
    setView('guide');
    window.scrollTo(0, 0);
  };
  const closeGuide = () => {
    location.hash = 'app';
    setView('app');
  };

  if (view === 'guide') return <Guide onBack={closeGuide} />;

  return (
    <div className="stage">
      <div className="phone">
        <StatusBar />
        <Route />
        <ConnectSheet />
        <Toasts />
        {s.ui.demoOpen && (
          <Sheet label="ควบคุมเดโม" onClose={() => updateUi(() => ({ demoOpen: false }))}>
            <DemoPanel onOpenGuide={openGuide} />
          </Sheet>
        )}
      </div>
      <aside className="side" aria-label="ควบคุมเดโม">
        <DemoPanel onOpenGuide={openGuide} />
      </aside>
      <Btn kind="demo" size="sm" icon="flask-2" className="demo-fab" onClick={() => updateUi(() => ({ demoOpen: true }))} style={{ boxShadow: '0 4px 14px rgba(0,0,0,.18)' }}>
        เดโม
      </Btn>
    </div>
  );
}
